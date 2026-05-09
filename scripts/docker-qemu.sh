#!/usr/bin/env sh
set -eu

docker build -t icda-toolchain .
docker run --rm -it -v "$PWD:/workspace" -w /workspace icda-toolchain make qemu
