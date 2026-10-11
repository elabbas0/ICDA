#!/bin/sh
# Builds icda-llm (the assistant's local model) as one static Linux binary for
# ICDA's Linux personality: llama.cpp for x86-64 with AVX2/FMA (Haswell and
# newer), in an Alpine (musl) container.
#   userspace/llm/build.sh [llama.cpp source dir]  ->  userspace/llm/out/icda-llm
# The source dir defaults to .verify/llama/src (a clone of ggml-org/llama.cpp).
set -e
cd "$(dirname "$0")/../.."
SRC=${1:-.verify/llama/src}
mkdir -p userspace/llm/out
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c "
    apk add -q build-base cmake linux-headers >/dev/null
    cmake -S $SRC -B $SRC/build-icda -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON \
        -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
        -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_TOOLS=OFF >/dev/null
    cmake --build $SRC/build-icda --target llama -j 2 >/dev/null
    g++ -O2 -static -std=c++17 -I$SRC/include -I$SRC/ggml/include userspace/llm/icda-llm.cpp \
        \$(find $SRC/build-icda -name 'libllama.a') \$(find $SRC/build-icda -name 'libggml*.a' | sort -r) \
        -o userspace/llm/out/icda-llm -lpthread
    strip userspace/llm/out/icda-llm
"
ls -la userspace/llm/out/icda-llm
