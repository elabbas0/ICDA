#!/usr/bin/env sh
set -eu

docker build -t icda-toolchain .
docker run --rm -v "$PWD:/workspace" -w /workspace icda-toolchain make
