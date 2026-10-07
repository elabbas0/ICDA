#!/bin/sh
# Builds the Linux-personality tests as static musl binaries (Alpine container).
# usage: sh tests/linux/build.sh   -> tests/linux/out/<name>
cd "$(dirname "$0")/../.."
mkdir -p tests/linux/out
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
  apk add --no-cache gcc musl-dev >/dev/null &&
  for f in tests/linux/*.c; do
    n=$(basename "$f" .c)
    gcc -static -O2 -pthread -o "tests/linux/out/$n" "$f" || exit 1
    echo "built tests/linux/out/$n"
  done'
