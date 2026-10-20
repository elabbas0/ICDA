#!/bin/sh
# Builds web/kernel.iso: a BIOS-only boot image with a gzip-compressed kernel
# for the in-browser demo.  GRUB decompresses the kernel while loading it, so
# the download is about a third of kernel.iso.
set -eu

kernel="${1:-kernel.bin}"
out="${2:-web/kernel.iso}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/boot/grub"
gzip -9c "$kernel" > "$work/boot/kernel.bin"
cp boot/grub/grub.cfg "$work/boot/grub/grub.cfg"
grub-mkrescue -d /usr/lib/grub/i386-pc \
    --install-modules="multiboot2 gzio part_msdos iso9660 search search_fs_file all_video gfxterm normal" \
    -o "$out" "$work"
