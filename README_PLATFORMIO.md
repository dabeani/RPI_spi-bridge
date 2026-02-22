# PlatformIO usage

This repository can be used as a PlatformIO project to trigger builds via `pio run` custom targets.

## Install PlatformIO

```bash
python3 -m pip install -U platformio
```

## Build kernel module

Requires kernel headers installed on the host (see main README).

```bash
pio run -t kmod
```

## Clean module artifacts

```bash
pio run -t kmodclean
```

## Build Debian package

```bash
pio run -t deb
```

This runs `./build-deb.sh` which uses `dpkg-buildpackage`.
