#!/bin/sh
# /swapfile tests: the FAT side (tests/fat/swapfile.c) on 4 KB and 32 KB
# clusters, then swapfile.c's start-up (tests/fat/swapfile-boot.c).
# usage: swapfile.sh   (needs gcc and dosfstools; in alpine they are added)
set -e
command -v apk >/dev/null 2>&1 && apk add -q build-base dosfstools >/dev/null 2>&1
cd "$(dirname "$0")/../.."
gcc -O1 -g -DHOST_TEST -Wno-unused -Wno-unused-result -I. -o /tmp/fatswapfile tests/fat/swapfile.c tests/fat/stubs.c kernel/fs/fatfs.c
for spc in 8 64; do
    rm -f /tmp/swapfile.img; truncate -s 3G /tmp/swapfile.img; mkfs.fat -F 32 -s $spc /tmp/swapfile.img >/dev/null
    echo "== $((spc / 2)) KB clusters"
    /tmp/fatswapfile /tmp/swapfile.img
done
gcc -O1 -g -DHOST_TEST -Wno-unused -Wno-unused-result -I. -o /tmp/fatswapboot tests/fat/swapfile-boot.c kernel/fs/swapfile.c \
    tests/fat/stubs.c kernel/fs/fatfs.c
rm -f /tmp/swapfile.img; truncate -s 5G /tmp/swapfile.img; mkfs.fat -F 32 -s 8 /tmp/swapfile.img >/dev/null
echo "== start-up"
/tmp/fatswapboot /tmp/swapfile.img
rm -f /tmp/swapfile.img
