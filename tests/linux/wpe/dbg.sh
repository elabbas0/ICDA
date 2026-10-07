#!/bin/sh
export HOME=/home XDG_RUNTIME_DIR=/home WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EGL_PLATFORM=wayland GSETTINGS_BACKEND=memory
/volumes/fat32-3/wpe-icda "${1:-http://example.com}" > /volumes/fat32-3/wpe.log 2>&1 &
B=/volumes/fat32-3/linux/bin/busybox
$B sleep 90; $B cat /dev/lxdump; $B sleep 30; $B cat /dev/lxdump
