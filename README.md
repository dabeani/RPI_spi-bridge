Would be nice if you support me: https://buymeacoffee.com/bmks — thank you very much!!

Important: Use at your own risk. You, the device owner, are responsible for any damage, data loss, or bricked devices.

# spi-bridge

Creates **multiple virtual device nodes** (e.g. `/dev/spi-bridge<BUS>.<0..N-1>`) that serialize and queue SPI operations.
By default all virtual nodes use one backing SPI device (for strict single-device arbitration), and optionally each virtual index can map to its own chip-select.

## Pre-build requirements (apt-get)

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  dkms \
  kmod \
  debhelper \
  devscripts
```

Kernel headers are required for DKMS builds:

### Raspberry Pi OS
```bash
sudo apt-get install -y raspberrypi-kernel-headers
```

### Generic Debian
```bash
sudo apt-get install -y linux-headers-$(uname -r)
```

## Build .deb (package builder)

From the project root:

```bash
./build-deb.sh
```

Artifacts are written to the parent directory.

## Install

```bash
sudo dpkg -i ../spi-bridge_1.1-1_all.deb
```

## Configure

Edit:

- `/etc/spi-bridge/bridge.conf`

Example:

```ini
BACKING=/dev/spidev0.0
NDEV=6
DEVNAME=spi-bridge
BUS=0
TIMEOUT_MS=30000
PER_MINOR_BACKING=0
OWNER_HOLD_MS=5
```

### Recommended mode: two applications, one physical SPI slave (`/dev/spidev0.0`)

Use this when app1 and app2 should both talk to the same slave via different virtual nodes:

```ini
BACKING=/dev/spidev0.0
BUS=0
NDEV=4
PER_MINOR_BACKING=0
OWNER_HOLD_MS=5
```

Then run app1 on `/dev/spi-bridge0.1` and app2 on `/dev/spi-bridge0.2`.

- `OWNER_HOLD_MS=0`: pure FIFO switching on every operation
- `OWNER_HOLD_MS=5..20`: reduces harmful interleaving between clients on the same slave
- increase `OWNER_HOLD_MS` if two clients still disturb each other

Apply:

```bash
sudo systemctl restart spi-bridge.service
```

### Map each virtual node to a different CS

If you want `/dev/spi-bridge0.1` and `/dev/spi-bridge0.2` to target different chip-selects, set:

```ini
BUS=0
PER_MINOR_BACKING=1
```

Then mappings are:

- `/dev/spi-bridge0.0` -> `/dev/spidev0.0`
- `/dev/spi-bridge0.1` -> `/dev/spidev0.1`
- `/dev/spi-bridge0.2` -> `/dev/spidev0.2`

Use this only when those are truly different chip-select devices.

## Verify

```bash
ls -l /dev/spi-bridge*.*
lsmod | grep spibridge
dmesg | tail -n 50
```

## Permissions

Devices are owned by group `spi` (created on install). Add your user:

```bash
sudo usermod -aG spi $USER
newgrp spi
```

## How queueing works

Each operation (`read`, `write`, `ioctl`) enters a **strict FIFO ticket queue**.
Only the current ticket may execute against the backing `/dev/spidevX.Y`.
This prevents collisions and interleaving at the SPI-device level.

With `OWNER_HOLD_MS > 0`, the active client gets a short temporary ownership window.
This keeps short request bursts together and improves stability for stateful protocols on one shared slave.

## Troubleshooting

### One app works, two apps fail on shared backing

1. Ensure shared mode is enabled:

```ini
PER_MINOR_BACKING=0
BACKING=/dev/spidev0.0
```

2. Start with:

```ini
OWNER_HOLD_MS=5
```

3. If unstable, try `OWNER_HOLD_MS=10` or `20`.
4. If startup appears blocked, temporarily set `OWNER_HOLD_MS=0` and compare behavior.
5. Restart service after each change:

```bash
sudo systemctl restart spi-bridge.service
```

6. Verify current settings and module state:

```bash
cat /etc/spi-bridge/bridge.conf
lsmod | grep spibridge
dmesg --ctime | tail -n 100
```

## Uninstall / purge

```bash
sudo apt-get remove spi-bridge
sudo apt-get purge spi-bridge
```
