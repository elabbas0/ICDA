#!/bin/sh
# Builds icda-stt (the assistant's speech recognition) as one static Linux
# binary for ICDA's Linux personality: whisper.cpp for x86-64 with AVX2/FMA, in
# Alpine.   userspace/llm/build-stt.sh [whisper.cpp source]  ->  userspace/llm/out/icda-stt
# The source defaults to .verify/whisper/src (a clone of ggml-org/whisper.cpp).
set -e
cd "$(dirname "$0")/../.."
SRC=${1:-.verify/whisper/src}
mkdir -p userspace/llm/out
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c "
    apk add -q build-base cmake linux-headers >/dev/null
    cmake -S $SRC -B $SRC/build-icda -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON \
        -DGGML_OPENMP=OFF -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_SERVER=OFF >/dev/null
    cmake --build $SRC/build-icda --target whisper -j 2 >/dev/null
    g++ -O2 -static -x c userspace/llm/icda-stt.c -x none -I$SRC/include -I$SRC/ggml/include \
        -Wl,--start-group \$(find $SRC/build-icda -name 'lib*.a') -Wl,--end-group \
        -o userspace/llm/out/icda-stt -lpthread
    strip userspace/llm/out/icda-stt
"
ls -la userspace/llm/out/icda-stt
