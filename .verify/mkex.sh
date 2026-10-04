set -e
out=${1:-.verify/disk-ex.img}
img=/tmp/disk-ex.img
rm -f "$img"
truncate -s 2G "$img"
sgdisk -Z "$img" >/dev/null
sgdisk -n1:2048:+128M -t1:ef00 -c1:"EFI system partition" \
       -n2:0:+600M -t2:0700 -c2:"USBDATA" \
       -n3:0:+400M -t3:0700 -c3:"Spare" "$img" >/dev/null
s1=$(sgdisk -i1 "$img" | awk '/First sector/{print $3}')
s2=$(sgdisk -i2 "$img" | awk '/First sector/{print $3}'); e2=$(sgdisk -i2 "$img" | awk '/Last sector/{print $3}')
truncate -s 128M /tmp/esp.img && mkfs.fat -F 32 -n ESP /tmp/esp.img >/dev/null
dd if=/tmp/esp.img of="$img" bs=1M seek=$((s1 / 2048)) conv=notrunc status=none
truncate -s $(( (e2 - s2 + 1) * 512 )) /tmp/ex.img
mkfs.exfat -L USBDATA /tmp/ex.img >/dev/null
mkdir -p /mnt/ex && mount -o loop /tmp/ex.img /mnt/ex
gcc -O2 -o /tmp/genrange .verify/genrange.c && /tmp/genrange > /mnt/ex/rangetest.bin
mkdir -p "/mnt/ex/Photos 2026/Trips/Summer"
printf 'small file\n' > "/mnt/ex/Photos 2026/Read me first.txt"
printf 'unicode name\n' > "/mnt/ex/café notes.txt"
printf 'deep\n' > "/mnt/ex/Photos 2026/Trips/Summer/beach.txt"
umount /mnt/ex
dd if=/tmp/ex.img of="$img" bs=1M seek=$((s2 / 2048)) conv=notrunc status=none
sgdisk -p "$img" | tail -3
cp --sparse=always "$img" "$out"
