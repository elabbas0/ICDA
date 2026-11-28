#!/bin/sh
# Builds .verify/linuxdisk.img: a FAT32 disk whose "linux" folder is a Linux
# root made from Alpine packages (the libraries dynamically linked programs
# need), plus the dynamic tests at the top.  ICDA mounts it under /volumes
# and Linux programs find /lib, /usr ... there (kernel/linux/lx.c overlay).
# usage: sh tests/linux/mkroot.sh [extra alpine packages...]
#
# The root is what a release ships (scripts/ota-release.sh publishes it, and
# the USB stick's "linux" folder is a copy): Surfer's WebKit engine with
# GStreamer's media, and the Windows layer.  A release built from a root
# without them removes them from every system that updates (1.11.1 and
# 1.11.2 did: Surfer fell back to its classic engine), so they are always
# in it; ICDA_ROOT=minimal makes a small root for tests of the Linux
# personality alone, which is never to be released.
cd "$(dirname "$0")/../.."
# Surfer's WebKit is not Alpine's (2.40.5, from 2023): userspace/webkit/engine
# builds a current WPE WebKit, put into the root with the Alpine packages its
# libraries need (engine-<version>.deps); mimalloc2 is the allocator Surfer
# gives WebKit's processes (browser.c)
. userspace/webkit/engine/versions.sh
ENGINE=userspace/webkit/out/engine-$WEBKIT.tar.gz
RELEASE_PKGS="ca-certificates font-dejavu glib-networking gsettings-desktop-schemas gstreamer gst-libav
  gst-plugins-bad gst-plugins-base gst-plugins-good mesa-dri-gallium mesa-egl mesa-gbm mesa-gles mimalloc2"
if [ "${ICDA_ROOT:-}" = minimal ]; then
    PKGS_ALL="$*"
    ENGINE=
else
    # Surfer (icda-webkit, the release's icda-surfer) and its engine are built first when they are not
    [ -f userspace/webkit/out/icda-webkit ] && [ -f $ENGINE ] || sh userspace/webkit/build.sh
    [ -f userspace/webkit/out/icda-webkit ] || { echo "mkroot: icda-webkit did not build (userspace/webkit/build.sh)"; exit 1; }
    [ -f $ENGINE ] || { echo "mkroot: the engine did not build (userspace/webkit/engine/build.sh)"; exit 1; }
    PKGS_ALL="$(echo $RELEASE_PKGS) $(cat userspace/webkit/out/engine-$WEBKIT.deps) $*"
fi
# Windows programs: the Windows layer (icda-win) ships in every root, so it is
# built first when it is not
[ -f userspace/winlayer/out/icda-win ] || sh userspace/winlayer/build.sh
[ -f userspace/winlayer/out/icda-win ] || { echo "mkroot: icda-win did not build (userspace/winlayer/build.sh)"; exit 1; }
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -e PKGS="$PKGS_ALL" -e ENGINE="$ENGINE" -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
apk add --no-cache gcc musl-dev zlib-dev dosfstools mtools sfdisk >/dev/null
R=/tmp/root
# the Windows layer needs Mesa'"'"'s OSMesa (OpenGL), libmspack (.cab archives)
# and FreeType (DirectWrite'"'"'s fonts)
PKGS="$PKGS mesa-osmesa libmspack freetype"
apk -X https://dl-cdn.alpinelinux.org/alpine/v3.20/main -X https://dl-cdn.alpinelinux.org/alpine/v3.20/community \
    -U --allow-untrusted --root $R --initdb add musl zlib busybox $PKGS >/dev/null
# Surfer'"'"'s engine: its libraries and processes (not its headers)
if [ -n "$ENGINE" ]; then
  tar -xzf $ENGINE -C $R
  rm -rf $R/usr/include $R/usr/lib/pkgconfig
fi
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
# GStreamer plugin registry, so programs do not read every plugin on first use
# (icda-webkit sets GST_REGISTRY to it, with GST_REGISTRY_UPDATE=no).  Made
# here, while the root still has its links: after links.sh nothing in it can
# run, and the registry was silently missing from every root before 1.12.
if [ -d $R/usr/lib/gstreamer-1.0 ]; then
  apk -X https://dl-cdn.alpinelinux.org/alpine/v3.20/main -X https://dl-cdn.alpinelinux.org/alpine/v3.20/community \
      -U --allow-untrusted --root $R add gstreamer-tools >/dev/null
  [ -f userspace/webkit/out/libgsticda.so ] && cp userspace/webkit/out/libgsticda.so $R/usr/lib/gstreamer-1.0/
  GST_REGISTRY=/gst-registry.bin GST_REGISTRY_FORK=no chroot $R /usr/bin/gst-inspect-1.0 >/dev/null
  [ -s $R/gst-registry.bin ] || { echo "mkroot: no GStreamer registry"; exit 1; }
fi
# fontconfig lists /etc/fonts/conf.d, and ICDA follows links only when a path
# is opened: its links become copies of the rules they name (without them
# "serif" is no alias and WebKit aborted on pages with audio players)
chroot $R /bin/busybox sh -c '"'"'for l in /etc/fonts/conf.d/*; do [ -L "$l" ] || continue; t=$(/bin/busybox readlink -f "$l"); [ -f "$t" ] && /bin/busybox cp "$t" "$l.copy" && /bin/busybox mv -f "$l.copy" "$l"; done'"'"'
chroot $R /bin/busybox sh /links.sh
for d in lib usr etc; do [ -d $R/$d ] && cp -r $R/$d $S/ 2>/dev/null || true; done
cp $R/.symlinks $S/.symlinks
mkdir -p $S/bin && cp -L $R/bin/busybox $S/bin/busybox
rm -rf $S/usr/bin $S/usr/sbin $S/usr/share/man $S/usr/share/doc
mkdir -p $S/usr/bin
for b in $R/usr/bin/*; do [ -e "$b" ] && [ ! -L "$b" ] && cp "$b" $S/usr/bin/; done; true
[ -f userspace/webkit/out/icda-webkit ] && cp userspace/webkit/out/icda-webkit $S/usr/bin/icda-webkit || true
cp userspace/winlayer/out/icda-win $S/usr/bin/icda-win
[ -f userspace/webkit/out/libgsticda.so ] && cp userspace/webkit/out/libgsticda.so $S/usr/lib/gstreamer-1.0/ || true
# the GStreamer registry made above
if [ -f $R/gst-registry.bin ]; then
  mkdir -p $S/usr/lib/icda
  cp $R/gst-registry.bin $S/usr/lib/icda/gst-registry.bin
fi
# Surfer'"'"'s fontconfig rules (FONTCONFIG_FILE), for roots made before conf.d was copied
mkdir -p $S/usr/lib/icda && cp userspace/webkit/fonts.conf $S/usr/lib/icda/fonts.conf
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
mdir -i $IMG@@1048576 ::/
'
# the root works with what ships beside it (a release checks it again)
if [ -n "$ENGINE" ]; then
    sh tests/linux/check-root.sh .verify/linuxroot userspace/webkit/out/icda-webkit \
        userspace/webkit/out/libgsticda.so userspace/winlayer/out/icda-win
fi
