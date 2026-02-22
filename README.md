# spi-bridge

Creates **multiple virtual device nodes** (e.g. `/dev/spi-bridge<BUS>.<0..N-1>`) that **serialize** and **queue** all SPI operations onto **one backing** SPI device (e.g. `/dev/spidev0.0`).

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
```

Apply:

```bash
sudo systemctl restart spi-bridge.service
```

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
