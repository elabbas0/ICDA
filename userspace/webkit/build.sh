#!/bin/sh
# Builds icda-webkit (Surfer on WebKit) against Alpine's WPE WebKit, with
# ICDA's window client and graphics compiled in (reaching the window
# manager through the kernel's gateway).  -> userspace/webkit/out/icda-webkit
cd "$(dirname "$0")/../.."
mkdir -p userspace/webkit/out
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
apk add --no-cache build-base pkgconf wpewebkit-dev libwpebackend-fdo-dev libwpe-dev glib-dev wayland-dev >/dev/null
PKG=$(pkg-config --list-all | grep -o "^wpe-webkit-[0-9.]*" | head -1)
gcc -O2 -Wall -Wno-unused-function -DICDA_SYS_BASE=0x1C000 -Iuserspace -o userspace/webkit/out/icda-webkit \
    userspace/webkit/browser.c userspace/webkit/ic_mem_libc.c userspace/gui.c userspace/ic_gfx.c userspace/ic_font.c \
    $(pkg-config --cflags --libs $PKG wpebackend-fdo-1.0 wpe-1.0 glib-2.0 gobject-2.0 wayland-server) -lm
echo "built userspace/webkit/out/icda-webkit ($PKG)"
'
