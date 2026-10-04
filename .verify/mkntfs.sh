set -e
out=${1:-.verify/disk-ntfs.img}
img=/tmp/disk-ntfs.img
rm -f "$img"
truncate -s 2G "$img"
sgdisk -Z "$img" >/dev/null
sgdisk -n1:2048:+128M -t1:ef00 -c1:"EFI system partition" \
       -n2:0:+900M -t2:0700 -c2:"Basic data partition" "$img" >/dev/null
s1=$(sgdisk -i1 "$img" | awk '/First sector/{print $3}')
s2=$(sgdisk -i2 "$img" | awk '/First sector/{print $3}'); e2=$(sgdisk -i2 "$img" | awk '/Last sector/{print $3}')
truncate -s 128M /tmp/esp.img && mkfs.fat -F 32 -n ESP /tmp/esp.img >/dev/null
dd if=/tmp/esp.img of="$img" bs=1M seek=$((s1 / 2048)) conv=notrunc status=none
truncate -s $(( (e2 - s2 + 1) * 512 )) /tmp/n.img
mkntfs -Q -F -L Windows /tmp/n.img >/dev/null 2>&1
mkdir -p /mnt/n && ntfs-3g /tmp/n.img /mnt/n
gcc -O2 -o /tmp/genrange .verify/genrange.c && /tmp/genrange > /mnt/n/rangetest.bin
printf 'tiny resident file\n' > /mnt/n/tiny.txt
mkdir -p "/mnt/n/Program Files/Common Files/System" "/mnt/n/Users/Public/Documents"
printf 'deep inside\n' > "/mnt/n/Program Files/Common Files/System/info.txt"
printf 'unicode name\n' > "/mnt/n/Users/Public/Documents/café notes.txt"
mkdir -p "/mnt/n/Windows/System32"
i=0; while [ $i -lt 3000 ]; do printf 'file %d\n' $i > "/mnt/n/Windows/System32/library-$i.dll"; i=$((i+1)); done
umount /mnt/n
dd if=/tmp/n.img of="$img" bs=1M seek=$((s2 / 2048)) conv=notrunc status=none
cp --sparse=always "$img" "$out"
echo done
