/* File: spibridge.c
 *
 * SPI bridge/multiplexer for Raspberry Pi:
 *  - Creates N virtual /dev nodes: /dev/<devname>0..N-1
 *  - Forwards all read/write/ioctl to ONE backing spidev device
 *  - Prevents collisions via strict FIFO queue (ticket-based), so calls are serialized in-order
 *
 * Notes:
 *  - This guarantees collision-free bus access (no concurrent SPI operations).
 *  - If two apps use a stateful higher-level protocol on the same device, they can still logically interfere.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/file.h>
#include <linux/atomic.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>
#include <linux/jiffies.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("spi-bridge");
MODULE_DESCRIPTION("SPI /dev bridge: multiple virtual dev nodes -> one backing spidev with strict FIFO queueing");
MODULE_VERSION("1.1");

/* -------------------- Module parameters -------------------- */

static char *backing = (char *)"/dev/spidev0.0";
module_param(backing, charp, 0644);
MODULE_PARM_DESC(backing, "Backing spidev device path (e.g., /dev/spidev0.0)");

static int ndev = 4;
module_param(ndev, int, 0644);
MODULE_PARM_DESC(ndev, "Number of virtual devices to create");

static char *devname = (char *)"spi-bridge";
module_param(devname, charp, 0644);
MODULE_PARM_DESC(devname, "Base name for created devices (e.g., spi-bridge -> /dev/spi-bridge0.0 ..)");

static int bus = 0;
module_param(bus, int, 0644);
MODULE_PARM_DESC(bus, "Bus number used only for naming (e.g., bus=0 -> /dev/spi-bridge0.<n>)");

static bool per_minor_backing = false;
module_param(per_minor_backing, bool, 0644);
MODULE_PARM_DESC(per_minor_backing, "If true, each /dev/<devname><bus>.<n> maps to /dev/spidev<bus>.<n> instead of one shared backing");


static int timeout_ms = 30000;
module_param(timeout_ms, int, 0644);
MODULE_PARM_DESC(timeout_ms, "Max time to wait in the FIFO queue per operation (ms), <=0 means wait forever");

static bool debug = false;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug logging for spibridge");

static int owner_hold_ms = 5;
module_param(owner_hold_ms, int, 0644);
MODULE_PARM_DESC(owner_hold_ms, "Keep one virtual client as temporary owner for this many ms to reduce cross-client interleaving on shared backing; 0 disables");

/* -------------------- Data structures -------------------- */

struct spibridge_fh {
	struct file *backing_filp;
};

struct spibridge_dev {
	struct cdev cdev;
	dev_t devno;
};

static dev_t g_base_devno;
static struct class *g_class;
static struct spibridge_dev *g_devs;

/* Strict FIFO queue state */
static atomic64_t g_next_ticket = ATOMIC64_INIT(0);
static atomic64_t g_serving    = ATOMIC64_INIT(0);
static wait_queue_head_t g_wq;

static struct spibridge_fh *g_owner_fh;
static unsigned long g_owner_until;
static DEFINE_SPINLOCK(g_owner_lock);

/* Why: guard backing device execution window, not just queue position */
static DEFINE_MUTEX(g_exec_mutex);

/* -------------------- FIFO queue helpers -------------------- */

static void spibridge_queue_exit(u64 my_ticket);

static bool spibridge_owner_allows(struct spibridge_fh *fh)
{
	bool allowed = true;
	unsigned long flags;

	if (owner_hold_ms <= 0)
		return true;

	spin_lock_irqsave(&g_owner_lock, flags);
	if (g_owner_fh && time_after_eq(jiffies, g_owner_until))
		g_owner_fh = NULL;

	if (g_owner_fh && g_owner_fh != fh)
		allowed = false;
	spin_unlock_irqrestore(&g_owner_lock, flags);

	return allowed;
}

static void spibridge_owner_touch(struct spibridge_fh *fh)
{
	unsigned long flags;

	if (owner_hold_ms <= 0)
		return;

	spin_lock_irqsave(&g_owner_lock, flags);
	g_owner_fh = fh;
	g_owner_until = jiffies + msecs_to_jiffies(owner_hold_ms);
	spin_unlock_irqrestore(&g_owner_lock, flags);
}

static void spibridge_owner_release(struct spibridge_fh *fh)
{
	unsigned long flags;

	spin_lock_irqsave(&g_owner_lock, flags);
	if (g_owner_fh == fh)
		g_owner_fh = NULL;
	spin_unlock_irqrestore(&g_owner_lock, flags);
}

static int spibridge_queue_enter(struct spibridge_fh *fh, u64 *ticket_out)
{
	u64 my_ticket = (u64)atomic64_fetch_inc(&g_next_ticket);
	int pending_err = 0;

	*ticket_out = my_ticket;

	if (debug)
		pr_info("spibridge: ticket %llu acquired\n", my_ticket);

	for (;;) {
		long rc;

		if ((u64)atomic64_read(&g_serving) == my_ticket && spibridge_owner_allows(fh))
			break;

		if (timeout_ms <= 0) {
			rc = wait_event_interruptible(
				g_wq,
				((u64)atomic64_read(&g_serving) == my_ticket && spibridge_owner_allows(fh))
			);
		} else {
			rc = wait_event_interruptible_timeout(
				g_wq,
				((u64)atomic64_read(&g_serving) == my_ticket && spibridge_owner_allows(fh)),
				msecs_to_jiffies(timeout_ms)
			);
		}

		if ((u64)atomic64_read(&g_serving) == my_ticket && spibridge_owner_allows(fh))
			break;

		if (rc == 0) {
			if (!pending_err)
				pending_err = -ETIMEDOUT;
			if (debug)
				pr_info("spibridge: ticket %llu timeout while queued, waiting to retire ticket\n", my_ticket);
			continue;
		}

		if (rc < 0) {
			if (!pending_err)
				pending_err = (int)rc;
			if (debug)
				pr_info("spibridge: ticket %llu interrupted rc=%ld while queued, waiting to retire ticket\n", my_ticket, rc);
			continue;
		}
	}

	if (debug)
		pr_info("spibridge: ticket %llu granted\n", my_ticket);

	if (pending_err) {
		spibridge_queue_exit(my_ticket);
		return pending_err;
	}

	spibridge_owner_touch(fh);

	return 0;
}

static void spibridge_queue_exit(u64 my_ticket)
{
	/* Only advance if we're currently serving this ticket */
	if ((u64)atomic64_read(&g_serving) == my_ticket) {
		atomic64_inc(&g_serving);
		wake_up_all(&g_wq);
		if (debug)
			pr_info("spibridge: ticket %llu completed, now serving %llu\n", my_ticket, (u64)atomic64_read(&g_serving));
	}
}

/* -------------------- Backing forwarding helpers -------------------- */

static long spibridge_forward_ioctl(struct file *backing_filp, unsigned int cmd, unsigned long arg)
{
	const struct file_operations *fops;

	if (!backing_filp)
		return -ENODEV;

	fops = backing_filp->f_op;
	if (!fops)
		return -ENODEV;

	if (fops->unlocked_ioctl)
		return fops->unlocked_ioctl(backing_filp, cmd, arg);

	return -ENOTTY;
}

static ssize_t spibridge_forward_read(struct file *backing_filp, char __user *buf, size_t len)
{
	const struct file_operations *fops;

	if (!backing_filp)
		return -ENODEV;

	fops = backing_filp->f_op;
	if (!fops)
		return -ENODEV;

	if (fops->read)
		return fops->read(backing_filp, buf, len, &backing_filp->f_pos);

	return -EINVAL;
}

static ssize_t spibridge_forward_write(struct file *backing_filp, const char __user *buf, size_t len)
{
	const struct file_operations *fops;

	if (!backing_filp)
		return -ENODEV;

	fops = backing_filp->f_op;
	if (!fops)
		return -ENODEV;

	if (fops->write)
		return fops->write(backing_filp, buf, len, &backing_filp->f_pos);

	return -EINVAL;
}

#ifdef CONFIG_COMPAT
static long spibridge_forward_compat_ioctl(struct file *backing_filp, unsigned int cmd, unsigned long arg)
{
	const struct file_operations *fops;

	if (!backing_filp)
		return -ENODEV;

	fops = backing_filp->f_op;
	if (!fops)
		return -ENODEV;

	if (fops->compat_ioctl)
		return fops->compat_ioctl(backing_filp, cmd, arg);

	/* Fallback to unlocked_ioctl if compat not provided */
	if (fops->unlocked_ioctl)
		return fops->unlocked_ioctl(backing_filp, cmd, arg);

	return -ENOTTY;
}
#endif

/* -------------------- File operations -------------------- */

static int spibridge_open(struct inode *inode, struct file *file)
{
	struct spibridge_fh *fh = kzalloc(sizeof(*fh), GFP_KERNEL);
	const char *selected_backing = backing;
	char backing_path[64];
	int idx;
	if (!fh)
		return -ENOMEM;

	idx = iminor(inode) - MINOR(g_base_devno);
	if (idx < 0 || idx >= ndev) {
		kfree(fh);
		return -ENODEV;
	}

	if (per_minor_backing) {
		scnprintf(backing_path, sizeof(backing_path), "/dev/spidev%d.%d", bus, idx);
		selected_backing = backing_path;
	}

	fh->backing_filp = filp_open(selected_backing, file->f_flags, 0);
	if (IS_ERR(fh->backing_filp)) {
		int err = PTR_ERR(fh->backing_filp);
		if (debug)
			pr_info("spibridge: open failed idx=%d path=%s err=%d\n", idx, selected_backing, err);
		kfree(fh);
		return err;
	}

	if (debug)
		pr_info("spibridge: open idx=%d -> %s\n", idx, selected_backing);

	file->private_data = fh;
	return 0;
}

static int spibridge_release(struct inode *inode, struct file *file)
{
	struct spibridge_fh *fh = file->private_data;

	if (fh) {
		if (fh->backing_filp && !IS_ERR(fh->backing_filp))
			filp_close(fh->backing_filp, NULL);
		spibridge_owner_release(fh);
		kfree(fh);
	}

	return 0;
}

static ssize_t spibridge_read(struct file *file, char __user *buf, size_t len, loff_t *ppos)
{
	struct spibridge_fh *fh = file->private_data;
	u64 ticket;
	int rc;
	ssize_t ret;
	(void)ppos;

	if (!fh || !fh->backing_filp)
		return -ENODEV;

	rc = spibridge_queue_enter(fh, &ticket);
	if (rc)
		return rc;

	mutex_lock(&g_exec_mutex);
	ret = spibridge_forward_read(fh->backing_filp, buf, len);
	mutex_unlock(&g_exec_mutex);

	spibridge_queue_exit(ticket);
	return ret;
}

static ssize_t spibridge_write(struct file *file, const char __user *buf, size_t len, loff_t *ppos)
{
	struct spibridge_fh *fh = file->private_data;
	u64 ticket;
	int rc;
	ssize_t ret;
	(void)ppos;

	if (!fh || !fh->backing_filp)
		return -ENODEV;

	rc = spibridge_queue_enter(fh, &ticket);
	if (rc)
		return rc;

	mutex_lock(&g_exec_mutex);
	ret = spibridge_forward_write(fh->backing_filp, buf, len);
	mutex_unlock(&g_exec_mutex);

	spibridge_queue_exit(ticket);
	return ret;
}

static long spibridge_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct spibridge_fh *fh = file->private_data;
	u64 ticket;
	int rc;
	long ret;

	if (!fh || !fh->backing_filp)
		return -ENODEV;

	rc = spibridge_queue_enter(fh, &ticket);
	if (rc)
		return rc;

	mutex_lock(&g_exec_mutex);
	ret = spibridge_forward_ioctl(fh->backing_filp, cmd, arg);
	mutex_unlock(&g_exec_mutex);

	spibridge_queue_exit(ticket);
	return ret;
}

#ifdef CONFIG_COMPAT
static long spibridge_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct spibridge_fh *fh = file->private_data;
	u64 ticket;
	int rc;
	long ret;

	if (!fh || !fh->backing_filp)
		return -ENODEV;

	rc = spibridge_queue_enter(fh, &ticket);
	if (rc)
		return rc;

	mutex_lock(&g_exec_mutex);
	ret = spibridge_forward_compat_ioctl(fh->backing_filp, cmd, arg);
	mutex_unlock(&g_exec_mutex);

	spibridge_queue_exit(ticket);
	return ret;
}
#endif

static __poll_t spibridge_poll(struct file *file, poll_table *wait)
{
	struct spibridge_fh *fh = file->private_data;

	if (!fh || !fh->backing_filp)
		return EPOLLERR;

	if (!fh->backing_filp->f_op || !fh->backing_filp->f_op->poll)
		return EPOLLIN | EPOLLOUT;

	return fh->backing_filp->f_op->poll(fh->backing_filp, wait);
}

static const struct file_operations spibridge_fops = {
	.owner          = THIS_MODULE,
	.open           = spibridge_open,
	.release        = spibridge_release,
	.read           = spibridge_read,
	.write          = spibridge_write,
	.unlocked_ioctl = spibridge_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = spibridge_compat_ioctl,
#endif
	.poll           = spibridge_poll,
	.llseek         = noop_llseek,
};

/* -------------------- Module init/exit -------------------- */

static int __init spibridge_init(void)
{
	int ret, i;

	if (ndev <= 0 || ndev > 256)
		return -EINVAL;

	init_waitqueue_head(&g_wq);

	ret = alloc_chrdev_region(&g_base_devno, 0, ndev, devname);
	if (ret)
		return ret;

	g_class = class_create(devname);
	if (IS_ERR(g_class)) {
		ret = PTR_ERR(g_class);
		unregister_chrdev_region(g_base_devno, ndev);
		return ret;
	}

	g_devs = kcalloc(ndev, sizeof(*g_devs), GFP_KERNEL);
	if (!g_devs) {
		class_destroy(g_class);
		unregister_chrdev_region(g_base_devno, ndev);
		return -ENOMEM;
	}

	for (i = 0; i < ndev; i++) {
		dev_t devno = MKDEV(MAJOR(g_base_devno), MINOR(g_base_devno) + i);

		g_devs[i].devno = devno;
		cdev_init(&g_devs[i].cdev, &spibridge_fops);
		g_devs[i].cdev.owner = THIS_MODULE;

		ret = cdev_add(&g_devs[i].cdev, devno, 1);
		if (ret)
			goto fail;

		{
			struct device *d = device_create(g_class, NULL, devno, NULL, "%s%d.%d", devname, bus, i);
			if (IS_ERR(d)) {
				ret = PTR_ERR(d);
				goto fail;
			}
		}
	}

	pr_info("spibridge: loaded backing=%s ndev=%d timeout_ms=%d dev=/dev/%s%d.[0..%d]\n",
		backing, ndev, timeout_ms, devname, bus, ndev - 1);
	return 0;

fail:
	while (i-- > 0) {
		device_destroy(g_class, g_devs[i].devno);
		cdev_del(&g_devs[i].cdev);
	}
	kfree(g_devs);
	class_destroy(g_class);
	unregister_chrdev_region(g_base_devno, ndev);
	return ret;
}

static void __exit spibridge_exit(void)
{
	int i;

	for (i = 0; i < ndev; i++) {
		device_destroy(g_class, g_devs[i].devno);
		cdev_del(&g_devs[i].cdev);
	}

	kfree(g_devs);
	class_destroy(g_class);
	unregister_chrdev_region(g_base_devno, ndev);

	pr_info("spibridge: unloaded\n");
}

module_init(spibridge_init);
module_exit(spibridge_exit);
