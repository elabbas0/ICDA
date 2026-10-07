#!/bin/sh
# Builds .verify/linuxdisk.img: a FAT32 disk whose "linux" folder is a Linux
# root made from Alpine packages (the libraries dynamically linked programs
# need), plus the dynamic tests at the top.  ICDA mounts it under /volumes
# and Linux programs find /lib, /usr ... there (kernel/linux/lx.c overlay).
# usage: sh tests/linux/mkroot.sh [extra alpine packages...]
cd "$(dirname "$0")/../.."
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -e PKGS="$*" -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
apk add --no-cache gcc musl-dev zlib-dev dosfstools mtools sfdisk >/dev/null
R=/tmp/root
apk -X https://dl-cdn.alpinelinux.org/alpine/v3.20/main -X https://dl-cdn.alpinelinux.org/alpine/v3.20/community \
    -U --allow-untrusted --root $R --initdb add musl zlib busybox $PKGS >/dev/null
S=/tmp/stage/linux
mkdir -p $S
# links: FAT has none, so they are listed in .symlinks (link, tab, target)
# and ICDA follows them for Linux programs (kernel/linux/lx.c)
# (inside the new root: its links are absolute)
cat > $R/links.sh <<"EOS"
find() { /bin/busybox find "$@"; }; rm() { /bin/busybox rm "$@"; }; cp() { /bin/busybox cp "$@"; }; mv() { /bin/busybox mv "$@"; }
grep() { /bin/busybox grep "$@"; }; head() { /bin/busybox head "$@"; }; cut() { /bin/busybox cut "$@"; }; readlink() { /bin/busybox readlink "$@"; }
find /lib /usr /etc -type l | while read L; do echo "$L $(readlink -f "$L")"; done > /links
while read L T; do
  [ -e "$T" ] || continue
  case "$T" in /bin/busybox) continue;; esac
  rm -f "$L"
  printf "%s\t%s\n" "$L" "$T" >> /.symlinks
done < /links
rm -f /links /links.sh
EOS
# what package scripts would have done if they could not run
[ -x $R/usr/bin/glib-compile-schemas ] && chroot $R /usr/bin/glib-compile-schemas /usr/share/glib-2.0/schemas || true
[ -x $R/usr/bin/gio-querymodules ] && chroot $R /usr/bin/gio-querymodules /usr/lib/gio/modules || true
[ -x $R/usr/bin/fc-cache ] && chroot $R /usr/bin/fc-cache -s || true
chroot $R /bin/busybox sh /links.sh
for d in lib usr etc; do [ -d $R/$d ] && cp -r $R/$d $S/ 2>/dev/null || true; done
cp $R/.symlinks $S/.symlinks
mkdir -p $S/bin && cp -L $R/bin/busybox $S/bin/busybox
rm -rf $S/usr/bin $S/usr/sbin $S/usr/share/man $S/usr/share/doc
mkdir -p $S/usr/bin
for b in $R/usr/bin/*; do [ -e "$b" ] && [ ! -L "$b" ] && cp "$b" $S/usr/bin/; done; true
[ -f userspace/webkit/out/icda-webkit ] && cp userspace/webkit/out/icda-webkit $S/usr/bin/icda-webkit || true
gcc -O2 -o /tmp/stage/dyntest tests/linux/dyn/dyntest.c -lz -pthread
du -sh $S
rm -rf .verify/linuxroot && cp -r $S .verify/linuxroot
# a 2 GB disk: one FAT32 partition from 1 MiB
IMG=.verify/linuxdisk.img
rm -f $IMG
dd if=/dev/zero of=$IMG bs=1M count=0 seek=2048 2>/dev/null
echo "2048,,c" | sfdisk -q $IMG
mkfs.vfat -F 32 -S 512 --offset 2048 -n LINUXROOT $IMG >/dev/null
mcopy -s -i $IMG@@1048576 /tmp/stage/linux ::/linux
mcopy -i $IMG@@1048576 /tmp/stage/dyntest ::/dyntest
[ -f tests/linux/out/wpe-icda ] && mcopy -i $IMG@@1048576 tests/linux/out/wpe-icda ::/wpe-icda
mcopy -i $IMG@@1048576 tests/linux/wpe/run.sh ::/wpe.sh
mcopy -i $IMG@@1048576 tests/linux/wpe/dbg.sh ::/dbg.sh
mcopy -i $IMG@@1048576 tests/linux/cog.sh ::/cog.sh
mdir -i $IMG@@1048576 ::/
'
