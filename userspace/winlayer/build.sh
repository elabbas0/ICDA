#!/bin/sh
# Builds icda-win (ICDA's Windows layer) against Alpine's Mesa (OSMesa, for
# OpenGL) and libmspack (for .cab archives), with ICDA's window client
# compiled in.  -> userspace/winlayer/out/icda-win
# tests/linux/mkroot.sh puts it in the Linux root as /usr/bin/icda-win,
# where the kernel finds it for any Windows program.
cd "$(dirname "$0")/../.."
mkdir -p userspace/winlayer/out
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w/userspace/winlayer alpine:3.20 sh -c '
set -e
apk add --no-cache build-base mesa-dev libmspack-dev python3 >/dev/null
sh build-host.sh out/icda-win
strip out/icda-win
echo "built userspace/winlayer/out/icda-win"
'
