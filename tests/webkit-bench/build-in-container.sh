#!/bin/sh
# Builds icda-webkit for the benchmark from a source tree (a revision's
# userspace/), with ICDA's window manager emulated (host/icda_sys.h) instead
# of the kernel's gateway.  Same compiler and flags as userspace/webkit/build.sh.
#   build-in-container.sh <tree containing userspace/> <output binary>
set -e
SRC=$1
OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd)
B=$(mktemp -d)
cp "$SRC/userspace/gui.c" "$B/gui.c"           # gui.c's "icda_sys.h" is then the emulated one
cp "$HERE/host/icda_sys.h" "$B/icda_sys.h"
PKG=$(pkg-config --list-all | grep -o "^wpe-webkit-[0-9.]*" | head -1)
gcc -O2 -Wall -Wno-unused-function -I"$SRC/userspace" -o "$OUT" \
    "$SRC/userspace/webkit/browser.c" "$SRC/userspace/webkit/ic_mem_libc.c" "$B/gui.c" \
    "$SRC/userspace/ic_gfx.c" "$SRC/userspace/ic_font.c" \
    $(pkg-config --cflags --libs $PKG wpebackend-fdo-1.0 wpe-1.0 glib-2.0 gobject-2.0 wayland-server egl glesv2) -lm
rm -rf "$B"
echo "built $OUT ($PKG)"
