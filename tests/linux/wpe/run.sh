#!/bin/sh
export HOME=/home XDG_RUNTIME_DIR=/home WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe EGL_PLATFORM=wayland GSETTINGS_BACKEND=memory WPE_CONSOLE=1
export WEBKIT_DEBUG=Process,Loading,Network,Compositing G_MESSAGES_DEBUG=all
exec /volumes/fat32-3/wpe-icda "${1:-http://example.com}" > /volumes/fat32-3/wpe.log 2>&1
