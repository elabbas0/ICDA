#!/bin/sh
# Builds icda-say (the assistant's voice) as one static Linux binary for ICDA's
# Linux personality, against Alpine's Flite.  -> userspace/llm/out/icda-say
set -e
cd "$(dirname "$0")/../.."
mkdir -p userspace/llm/out
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c "
    apk add -q build-base flite-dev >/dev/null
    gcc -O2 -static userspace/llm/icda-say.c userspace/llm/alsa-stubs.c -o userspace/llm/out/icda-say \
        -lflite_cmu_us_slt -lflite_usenglish -lflite_cmulex -lflite -lm
    strip userspace/llm/out/icda-say
"
ls -la userspace/llm/out/icda-say
