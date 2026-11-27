#!/bin/sh
# Builds icda-webkit (Surfer on WebKit) against Surfer's engine, WPE WebKit
# built by engine/build.sh (built first when it is not: hours), with ICDA's
# window client and graphics compiled in (reaching the window manager through
# the kernel's gateway).  -> userspace/webkit/out/icda-webkit
cd "$(dirname "$0")/../.."
. userspace/webkit/engine/versions.sh
mkdir -p userspace/webkit/out
sh userspace/webkit/engine/build.sh || exit 1
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -e WEBKIT=$WEBKIT -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
# the headers the engine'"'"'s headers include and the libraries it needs, then the engine itself
apk add --no-cache build-base pkgconf glib-dev libsoup3-dev libxkbcommon-dev wayland-dev mesa-dev \
    $(cat userspace/webkit/out/engine-$WEBKIT.deps) >/dev/null
tar -xzf userspace/webkit/out/engine-$WEBKIT.tar.gz -C /
PKG=$(pkg-config --list-all | grep -o "^wpe-webkit-[0-9.]*" | head -1)
gcc -O2 -Wall -Wno-unused-function -DICDA_SYS_BASE=0x1C000 -Iuserspace -o userspace/webkit/out/icda-webkit \
    userspace/webkit/browser.c userspace/webkit/ic_mem_libc.c userspace/gui.c userspace/ic_gfx.c userspace/ic_font.c \
    $(pkg-config --cflags --libs $PKG wpebackend-fdo-1.0 wpe-1.0 glib-2.0 gobject-2.0 wayland-server egl glesv2) -lm
echo "built userspace/webkit/out/icda-webkit ($PKG $(pkg-config --modversion $PKG))"
'
# the GStreamer sound output for WebKit's media
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
apk add --no-cache build-base pkgconf gstreamer-dev gst-plugins-base-dev >/dev/null
gcc -O2 -Wall -shared -fPIC -DICDA_SYS_BASE=0x1C000 -Iuserspace -o userspace/webkit/out/libgsticda.so \
    userspace/webkit/gsticdasink.c $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-audio-1.0)
echo "built userspace/webkit/out/libgsticda.so"
'
