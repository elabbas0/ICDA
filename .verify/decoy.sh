# Adds a decoy ICDA partition to .verify/otadisk.img (the last partition, searched
# before the real one): ICDAROOT.BIN and a truncated KERNEL.BIN, no GRUB - what an
# interrupted earlier install leaves.  GRUB must still boot partition 4.
set -e
img=.verify/otadisk.img
start=$(sgdisk -i5 $img | awk '/First sector/{print $3}')
truncate -s 300M /tmp/decoy.img; mkfs.fat -F 32 -n ICDAROOT /tmp/decoy.img >/dev/null
mmd -i /tmp/decoy.img ::/EFI ::/EFI/ICDA
head -c 3000000 .verify/ota/EFI/ICDA/KERNEL.BIN > /tmp/KERNEL.BIN
mcopy -i /tmp/decoy.img /tmp/KERNEL.BIN ::/EFI/ICDA/KERNEL.BIN
mcopy -i /tmp/decoy.img .verify/deploy/ICDAROOT.BIN ::/ICDAROOT.BIN
dd if=/tmp/decoy.img of=$img bs=1M seek=$((start / 2048)) conv=notrunc status=none
echo "decoy written at sector $start"
