#!/bin/sh
set -e
mkdir -p .verify
for m in "$@"; do
    w=${m%x*}
    h=${m#*x}
    rm -rf /tmp/icda-res
    mkdir /tmp/icda-res
    tar --exclude=./.verify --exclude=./.git -cf - . | tar -xf - -C /tmp/icda-res
    (cd /tmp/icda-res && sed -i "30s/dd 0/dd $w/; 31s/dd 0/dd $h/" kernel/boot.asm &&
        make kernel.iso >/dev/null 2>&1)
    cp /tmp/icda-res/kernel.iso ".verify/kernel-$m.iso"
    echo "built .verify/kernel-$m.iso"
done
