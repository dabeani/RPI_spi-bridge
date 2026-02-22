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
```

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

## Uninstall / purge

```bash
sudo apt-get remove spi-bridge
sudo apt-get purge spi-bridge
```
