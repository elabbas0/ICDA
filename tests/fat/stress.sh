#!/bin/sh
# FAT32 stress test (tests/fat/stress.c) on 4 KB and 8 KB clusters - 8 KB is
# what Windows gives a 40+ GB partition.  usage (in alpine): stress.sh [rounds]
set -e
apk add -q build-base dosfstools >/dev/null 2>&1
cd "$(dirname "$0")/../.."
gcc -O1 -g -DHOST_TEST -Wno-unused -I. -o /tmp/fatstress tests/fat/stress.c tests/fat/stubs.c kernel/fs/fatfs.c
for spc in 8 16; do
    for seed in 1 2; do
        rm -f /tmp/s.img; truncate -s 700M /tmp/s.img; mkfs.fat -F 32 -s $spc /tmp/s.img >/dev/null
        printf '%s KB clusters, seed %s: ' $((spc / 2)) $seed
        /tmp/fatstress /tmp/s.img "${1:-40}" $seed
    done
done
