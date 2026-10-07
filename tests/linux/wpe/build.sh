#!/bin/sh
# Builds wpe-icda (WebKit in an ICDA window) against Alpine's WPE WebKit,
# with ICDA's window client (userspace/gui.c) reaching the window manager
# through the kernel's gateway.  -> tests/linux/out/wpe-icda
cd "$(dirname "$0")/../../.."
mkdir -p tests/linux/out
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
apk add --no-cache build-base pkgconf wpewebkit-dev libwpebackend-fdo-dev libwpe-dev glib-dev wayland-dev >/dev/null
PKG=$(pkg-config --list-all | grep -o "^wpe-webkit-[0-9.]*" | head -1)
gcc -O2 -Wall -DICDA_SYS_BASE=0x1C000 -Iuserspace -o tests/linux/out/wpe-icda tests/linux/wpe/wpe-icda.c userspace/gui.c \
    $(pkg-config --cflags --libs $PKG wpebackend-fdo-1.0 wpe-1.0 glib-2.0 gobject-2.0 wayland-server)
echo "built tests/linux/out/wpe-icda ($PKG)"
'
