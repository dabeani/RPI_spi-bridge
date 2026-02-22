#!/bin/sh
set -eu

if ! command -v dpkg-buildpackage >/dev/null 2>&1; then
  echo "dpkg-buildpackage not found. Install:"
  echo "  sudo apt-get install -y devscripts build-essential debhelper dkms"
  exit 1
fi

dpkg-buildpackage -us -uc -b
