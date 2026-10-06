#!/bin/sh
# Builds ICDA and prepares the dual-boot deploy files in .verify/deploy:
#   GRUBX64.EFI, KERNEL.BIN (installed kernel), ICDAROOT.BIN (system image), ICDACFG.TXT
# The system image is produced by ICDA itself: a live boot in QEMU finds the
# marked scratch disk and writes the packed system to it.
# Then run (elevated):  powershell -ExecutionPolicy Bypass -File scripts\icda-dualboot.ps1
set -e
cd "$(dirname "$0")/.."
QEMU=${QEMU:-/c/Users/elabbas/tools/qemu/qemu-system-x86_64.exe}
powershell -NoProfile -Command "Get-Process qemu-system-x86_64 -EA SilentlyContinue | Stop-Process -Force" || true
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/workspace" -w /workspace icda-toolchain sh -c 'make > /tmp/b.log 2>&1 || { tail -20 /tmp/b.log; exit 1; }'
mkdir -p .verify/deploy
rm -f .verify/scratch.img
truncate -s 128M .verify/scratch.img
printf 'ICDA-BUNDLE-SCRATCH-V1' | dd of=.verify/scratch.img conv=notrunc status=none
timeout 240 "$QEMU" -machine q35 -m 2G -smp 2 -cdrom kernel.iso -drive file=.verify/scratch.img,format=raw,media=disk \
    -display none -monitor none -serial file:.verify/serial-mk.log -no-reboot
n=$(head -c 64 .verify/scratch.img | sed -n 's/.*DONE bytes=\([0-9]*\).*/\1/p')
[ -n "$n" ] || { echo "system image export failed (see .verify/serial-mk.log)"; exit 1; }
dd if=.verify/scratch.img of=.verify/deploy/ICDAROOT.BIN bs=512 skip=1 count=$((n / 512)) status=none
printf 'swap=-1\n' > .verify/deploy/ICDACFG.TXT
cp kernel/fs/bootx64-install.efi .verify/deploy/GRUBX64.EFI
cp kernel/install-kernel.bin .verify/deploy/KERNEL.BIN
ls -la .verify/deploy
