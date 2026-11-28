#!/bin/sh
# GStreamer's plugin registry for the Linux root Surfer's WebKit runs from:
# what plugins there are and what each offers, so a program using GStreamer
# does not open every plugin (and fork gst-plugin-scanner) on first use.
# icda-webkit points GST_REGISTRY at it with GST_REGISTRY_UPDATE=no.
#
#   sh tests/linux/gst-registry.sh   -> userspace/webkit/out/gst-registry.bin
#
# Made in an Alpine 3.20 root with the release root's GStreamer packages and
# ICDA's sound output (libgsticda.so, userspace/webkit/build.sh); the root's
# links are kept here (mkroot.sh removes them for FAT, after which nothing in
# the root can run).  scripts/ota-release.sh ships it as
# linux/usr/lib/icda/gst-registry.bin.
set -e
cd "$(dirname "$0")/../.."
[ -f userspace/webkit/out/libgsticda.so ] || { echo "gst-registry: build userspace/webkit/out/libgsticda.so first (userspace/webkit/build.sh)"; exit 1; }
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
R=/tmp/root
apk -X https://dl-cdn.alpinelinux.org/alpine/v3.20/main -X https://dl-cdn.alpinelinux.org/alpine/v3.20/community \
    -U --allow-untrusted --root $R --initdb add musl busybox gstreamer gstreamer-tools gst-plugins-base \
    gst-plugins-good gst-plugins-bad gst-libav >/dev/null
cp userspace/webkit/out/libgsticda.so $R/usr/lib/gstreamer-1.0/
GST_REGISTRY=/gst-registry.bin GST_REGISTRY_FORK=no chroot $R /usr/bin/gst-inspect-1.0 > /tmp/inspect.txt
grep -q "^icda:" /tmp/inspect.txt || { echo "gst-registry: ICDA sound output not in it"; exit 1; }
cp $R/gst-registry.bin userspace/webkit/out/gst-registry.bin
echo "gst-registry: $(grep -c "" /tmp/inspect.txt) features, $(du -h $R/gst-registry.bin | cut -f1)"
'
