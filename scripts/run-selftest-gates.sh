








set -eu

image="${ICDA_DOCKER_IMAGE:-icda-toolchain}"
gate() {
    name="$1"
    flag="$2"
    echo "=== gate: $name ($flag) ==="
    
    
    
    
    
    pristine="$(cat boot/grub/grub.cfg)"
    trap 'printf "%s\n" "$pristine" > boot/grub/grub.cfg' EXIT INT TERM HUP
    sed -i "s|multiboot2 /boot/kernel.bin icda.live=1|multiboot2 /boot/kernel.bin icda.live=1 icda.test=$flag|" boot/grub/grub.cfg
    make clean >/dev/null 2>&1 || true
    make kernel.iso CI_SELFTEST=1 CI_IMAGE=1 >/dev/null
    QEMU_LOG="/tmp/$flag.log" QEMU_TIMEOUT="${QEMU_TIMEOUT:-300}" \
        sh scripts/qemu-smoke.sh kernel.iso || true
    if grep -aq "NPTEST DONE ALL-PASS" "/tmp/$flag.log"; then
        echo "gate $name: PASS"
        rc=0
    else
        echo "gate $name: FAIL (no ALL-PASS marker)"
        grep -a "NPTEST\|nptest" "/tmp/$flag.log" | tail -20 || echo "(no nptest output at all)"
        rc=1
    fi
    printf "%s\n" "$pristine" > boot/grub/grub.cfg
    trap - EXIT INT TERM HUP
    return $rc
}

fail=0
gate nptest nptest || fail=1
gate nptestlx nptestlx || fail=1
exit "$fail"
