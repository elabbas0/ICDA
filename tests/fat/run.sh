#!/bin/sh
# FAT32 code test: builds tests/fat/fattest against kernel/fs/fatfs.c, copies a
# tree into a fresh FAT32 image like the installer does, checks it with
# fsck.fat and compares every file.  usage (in alpine): run.sh <source dir> [MB]
set -e
apk add -q build-base dosfstools >/dev/null 2>&1
cd "$(dirname "$0")/../.."
src=$1; mb=${2:-1024}
gcc -O1 -g -DHOST_TEST -Wno-unused -I. -o /tmp/fattest tests/fat/fattest.c tests/fat/stubs.c kernel/fs/fatfs.c
rm -f /tmp/fat.img; truncate -s ${mb}M /tmp/fat.img; mkfs.fat -F 32 -n TEST /tmp/fat.img >/dev/null
/tmp/fattest /tmp/fat.img "$src" /linux
fsck.fat -n /tmp/fat.img | tail -4
mkdir -p /tmp/m; apk add -q mtools >/dev/null 2>&1
mcopy -s -i /tmp/fat.img ::/linux /tmp/m/ 2>/dev/null || true
diff -rq "$src" /tmp/m/linux | head -10; echo "diff done"
