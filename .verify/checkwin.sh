img=${1:-.verify/disk-win.img}
s1=$(sgdisk -i1 "$img" | awk '/First sector/{print $3}'); e1=$(sgdisk -i1 "$img" | awk '/Last sector/{print $3}')
s3=$(sgdisk -i3 "$img" | awk '/First sector/{print $3}'); e3=$(sgdisk -i3 "$img" | awk '/Last sector/{print $3}')
dd if="$img" of=/tmp/esp.img bs=512 skip=$s1 count=$((e1 - s1 + 1)) status=none iflag=fullblock 2>/dev/null || dd if="$img" of=/tmp/esp.img bs=1M skip=$((s1/2048)) count=$(((e1-s1+1)/2048)) status=none
dd if="$img" of=/tmp/root.img bs=1M skip=$((s3/2048)) count=$(((e3-s3+1)/2048)) status=none
echo "== GPT"; sgdisk -p "$img" | tail -3
echo "== fsck ESP"; fsck.fat -n /tmp/esp.img | tail -2
echo "== fsck ROOT"; fsck.fat -n /tmp/root.img | tail -2
echo "== ESP tree"; mdir -/ -i /tmp/esp.img ::/ | grep -v "^$\|Volume\|Directory for\|bytes free\|files\b" | head -30
echo "== windows files"; mtype -i /tmp/esp.img ::/EFI/Microsoft/Boot/bootmgfw.efi; mtype -i /tmp/esp.img ::/EFI/Boot/bootx64.efi
echo "== root"; mdir -i /tmp/root.img ::/ | grep -i icda
