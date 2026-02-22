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


static int timeout_ms = 30000;
module_param(timeout_ms, int, 0644);
MODULE_PARM_DESC(timeout_ms, "Max time to wait in the FIFO queue per operation (ms), <=0 means wait forever");

static bool debug = false;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug logging for spibridge");

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

/* Why: guard backing device execution window, not just queue position */
static DEFINE_MUTEX(g_exec_mutex);

/* -------------------- FIFO queue helpers -------------------- */

static int spibridge_queue_enter(u64 *ticket_out)
{
	u64 my_ticket = (u64)atomic64_fetch_inc(&g_next_ticket);

	*ticket_out = my_ticket;

	if (debug)
		pr_info("spibridge: ticket %llu acquired\n", my_ticket);

	if (timeout_ms <= 0) {
		int rc = wait_event_interruptible(g_wq, (u64)atomic64_read(&g_serving) == my_ticket);
		if (debug)
			pr_info("spibridge: ticket %llu wait returned rc=%d\n", my_ticket, rc);
		return rc; /* 0 or -ERESTARTSYS */
	}

	{
		long t = msecs_to_jiffies(timeout_ms);
		long rc = wait_event_interruptible_timeout(
			g_wq,
			(u64)atomic64_read(&g_serving) == my_ticket,
			t
		);

		if (rc > 0)
			if (debug)
				pr_info("spibridge: ticket %llu granted (timeout)\n", my_ticket);
			return 0;
		if (rc == 0)
			return -ETIMEDOUT;
		return (int)rc; /* -ERESTARTSYS */
	}
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
	if (!fh)
		return -ENOMEM;

	fh->backing_filp = filp_open(backing, file->f_flags, 0);
	if (IS_ERR(fh->backing_filp)) {
		int err = PTR_ERR(fh->backing_filp);
		kfree(fh);
		return err;
	}

	file->private_data = fh;
	return 0;
}

static int spibridge_release(struct inode *inode, struct file *file)
{
	struct spibridge_fh *fh = file->private_data;

	if (fh) {
		if (fh->backing_filp && !IS_ERR(fh->backing_filp))
			filp_close(fh->backing_filp, NULL);
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

	if (!fh || !fh->backing_filp)
		return -ENODEV;

	rc = spibridge_queue_enter(&ticket);
	if (rc)
		return rc;

	mutex_lock(&g_exec_mutex);
	ret = kernel_read(fh->backing_filp, buf, len, ppos);
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

	if (!fh || !fh->backing_filp)
		return -ENODEV;

	rc = spibridge_queue_enter(&ticket);
	if (rc)
		return rc;

	mutex_lock(&g_exec_mutex);
	ret = kernel_write(fh->backing_filp, buf, len, ppos);
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

	rc = spibridge_queue_enter(&ticket);
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

	rc = spibridge_queue_enter(&ticket);
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
