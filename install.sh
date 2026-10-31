#!/usr/bin/env bash
# Installation script for ms912x driver with DKMS support
set -e

if [ "$EUID" -ne 0 ]; then
    echo "Error: please run as root: sudo bash $0"
    exit 1
fi

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KVER="$(uname -r)"

echo "==> [1/4] Building kernel module for $KVER..."
make -C "$DIR" clean
make -C "$DIR" all -j"$(nproc)"

echo "==> [2/4] Installing module and updating dependencies..."
mkdir -p "/lib/modules/$KVER/updates"
cp "$DIR/ms912x.ko" "/lib/modules/$KVER/updates/"
depmod -a "$KVER"

echo "==> [3/4] Registering with DKMS for automatic rebuilds on kernel updates..."
mkdir -p /usr/src/ms912x-0.1
cp -r "$DIR"/* /usr/src/ms912x-0.1/
make -C /usr/src/ms912x-0.1 clean 2>/dev/null || true
rm -f /usr/src/ms912x-0.1/*.o /usr/src/ms912x-0.1/*.ko /usr/src/ms912x-0.1/.*.cmd /usr/src/ms912x-0.1/*.mod*

dkms remove -m ms912x -v 0.1 --all 2>/dev/null || true
dkms add -m ms912x -v 0.1
dkms build -m ms912x -v 0.1
dkms install -m ms912x -v 0.1 --force

echo "==> [4/4] Loading driver..."
modprobe drm_shmem_helper 2>/dev/null || true
modprobe ms912x

echo ""
echo "Driver successfully installed and loaded!"
lsmod | grep ms912x
