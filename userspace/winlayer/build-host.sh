#!/bin/sh
# Builds icda-win with the C library it runs on (musl, as ICDA's Linux side),
# inside Alpine with mesa-dev and mesa-osmesa: sh build-host.sh [out]
set -e
cd "$(dirname "$0")"
out=${1:-icda-win}
gen=$(mktemp -d)
python3 gen_gl.py /usr/include/GL/gl.h /usr/include/GL/glext.h > "$gen/gl_thunks.c"
CFLAGS="-O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fno-strict-aliasing -pthread -DICDA_SYS_BASE=0x1C000 -I. -I.."
gcc $CFLAGS -o "$out" main.c loader.c builtin.c thread.c util.c kernel32.c crt.c fmt.c misc.c ws2_32.c \
    user32.c gdi32.c opengl32.c winmm.c cabinet.c registry.c backend.c ../gui.c ../ic_font.c ../ic_gfx.c "$gen/gl_thunks.c" -lOSMesa -lmspack -lm
rm -rf "$gen"
