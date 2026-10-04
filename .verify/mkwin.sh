set -e
out=${1:-.verify/disk-win.img}
img=/tmp/disk-win.img
rm -f "$img" /tmp/esp.img /tmp/data.img
truncate -s 2G "$img"
sgdisk -Z "$img" >/dev/null
sgdisk -n1:2048:+128M -t1:ef00 -c1:"EFI system partition" \
       -n2:0:+400M -t2:0700 -c2:"Basic data partition" \
       -n3:0:+400M -t3:0700 -c3:"DATA" "$img" >/dev/null
start1=$(sgdisk -i1 "$img" | awk '/First sector/{print $3}')
start3=$(sgdisk -i3 "$img" | awk '/First sector/{print $3}')
truncate -s 128M /tmp/esp.img
mkfs.fat -F 32 -S 512 -s 1 -n ESP /tmp/esp.img >/dev/null
mmd -i /tmp/esp.img ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot ::/EFI/Boot
printf 'FAKE WINDOWS BOOT MANAGER\n' > /tmp/bootmgfw.efi
printf 'FAKE WINDOWS FALLBACK LOADER\n' > /tmp/bootx64.efi
mcopy -i /tmp/esp.img /tmp/bootmgfw.efi ::/EFI/Microsoft/Boot/bootmgfw.efi
mcopy -i /tmp/esp.img /tmp/bootx64.efi ::/EFI/Boot/bootx64.efi
mcopy -i /tmp/esp.img /tmp/bootx64.efi "::/EFI/Microsoft/Boot/A long file name.txt"
dd if=/tmp/esp.img of="$img" bs=1M seek=$((start1 / 2048)) conv=notrunc,sparse status=none
truncate -s 400M /tmp/data.img
mkfs.fat -F 32 -n ICDAROOT /tmp/data.img >/dev/null
dd if=/tmp/data.img of="$img" bs=1M seek=$((start3 / 2048)) conv=notrunc,sparse status=none
sgdisk -p "$img" | tail -4
cp --sparse=always "$img" "$out"
