#!/usr/bin/env sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run as root: sudo ./scripts/install-requirements.sh"
    exit 1
fi

if command -v apt-get >/dev/null 2>&1; then
    apt-get update
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        gcc \
        git \
        grub-common \
        grub-pc-bin \
        make \
        mtools \
        nasm \
        qemu-system-x86 \
        xorriso
else
    echo "Unsupported package manager. Install gcc, make, nasm, grub tools, xorriso, mtools, and qemu-system-x86_64."
    exit 1
fi
