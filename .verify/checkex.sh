img=${1:-.verify/disk-ex.img}
part=${2:-2}
s=$(sgdisk -i$part "$img" | awk '/First sector/{print $3}'); e=$(sgdisk -i$part "$img" | awk '/Last sector/{print $3}')
dd if="$img" of=/tmp/p.img bs=1M skip=$((s / 2048)) count=$(((e - s + 1) / 2048)) status=none
echo "== fsck.exfat p$part"; fsck.exfat -n /tmp/p.img 2>&1 | tail -1
mkdir -p /mnt/p && mount -o loop,ro /tmp/p.img /mnt/p || exit 1
echo "== tree"; cd /mnt/p && find . | sort | head -40
for f in "Photos 2026/Read me first.txt" "café notes.txt" "notes-from-icda.txt" "Photos 2026/Trips/Summer/beach.txt"; do
  [ -f "$f" ] && echo "== $f: $(cat "$f")"
done
if [ -f rangetest.bin ]; then gcc -O2 -o /tmp/g /workspace/.verify/genrange.c && /tmp/g | cmp - rangetest.bin && echo "rangetest.bin intact"; fi
cd / && umount /mnt/p
