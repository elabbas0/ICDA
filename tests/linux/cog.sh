#!/bin/sh
export HOME=/home XDG_RUNTIME_DIR=/home WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
export WPE_BACKEND_LIBRARY=libWPEBackend-fdo-1.0.so.1
exec /volumes/fat32-3/linux/usr/bin/cog --platform=headless "${1:-http://example.com}"
