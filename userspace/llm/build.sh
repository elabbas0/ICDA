#!/bin/sh
# Builds icda-llm (the assistant's local model) as one static Linux binary for
# ICDA's Linux personality: llama.cpp for x86-64 with AVX2/FMA (Haswell and
# newer), in an Alpine (musl) container.
#   userspace/llm/build.sh [llama.cpp source dir]  ->  userspace/llm/out/icda-llm
# The source dir defaults to .verify/llama/src (a clone of ggml-org/llama.cpp).
#
# A cold build compiles all of llama.cpp and takes a long time, so each stage
# reports itself: the bar follows cmake's own "[ NN%]" line, and the stage's
# full output stays in .verify/llm-build/ if it fails.
set -e
cd "$(dirname "$0")/../.."
. scripts/progress.sh
SRC=${1:-.verify/llama/src}
LOGS=.verify/llm-build
mkdir -p userspace/llm/out "$LOGS"

# The stages run inside the container, one docker run per stage so that each can
# be watched on its own.  cmake's build tree is on the volume, so the expensive
# compile is only paid once.
run_in_container() {
    # without this, Git Bash rewrites the container-side /w into a Windows path
    MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c "
        apk add -q build-base cmake linux-headers >/dev/null 2>&1
        $1"
}

spin_pct "llama.cpp" "$LOGS/configure.log" run_in_container "
    cmake -S $SRC -B $SRC/build-icda -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON \
        -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
        -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_TOOLS=OFF
"

spin_pct "compiling" "$LOGS/build.log" run_in_container "
    cmake --build $SRC/build-icda --target llama -j 2
"

spin "linking" "$LOGS/link.log" run_in_container "
    g++ -O2 -static -std=c++17 -I$SRC/include -I$SRC/ggml/include userspace/llm/icda-llm.cpp \
        \$(find $SRC/build-icda -name 'libllama.a') \$(find $SRC/build-icda -name 'libggml*.a' | sort -r) \
        -o userspace/llm/out/icda-llm -lpthread
    strip userspace/llm/out/icda-llm
"

note "icda-llm: $(wc -c < userspace/llm/out/icda-llm | tr -d ' ') bytes"
ls -la userspace/llm/out/icda-llm