#!/usr/bin/env sh
set -eu

iso="${1:-kernel.iso}"
log="${QEMU_LOG:-qemu-smoke.log}"
seconds="${QEMU_TIMEOUT:-8}"

rm -f "$log"

set +e
timeout --foreground "$seconds" \
    qemu-system-x86_64 \
    -cdrom "$iso" \
    -m 256M \
    -serial "file:$log" \
    -display none \
    -monitor none \
    -no-reboot \
    -no-shutdown
status=$?
set -e

if [ "$status" -ne 0 ] && [ "$status" -ne 124 ]; then
    echo "QEMU exited unexpectedly with status $status"
    [ -f "$log" ] && cat "$log"
    exit "$status"
fi

if grep -Eq "FAIL|OOM|PAGE FAULT|EXCEPTION" "$log"; then
    echo "QEMU smoke test failed: boot log contains an error marker."
    cat "$log"
    exit 1
fi

if \
    (grep -q "INTERRUPTS: OK" "$log" && grep -q "Scheduler running" "$log") \
    || \
    (grep -q "\\[boot\\] interrupts: Local APIC / IOAPIC active" "$log" \
        && grep -q "\\[boot\\] scheduler: scheduler core online" "$log" \
        && grep -q "\\[boot\\] tty: starting interactive console" "$log" \
        && grep -q "welcome to icda" "$log"); then
    cat "$log"
    echo
    echo "QEMU smoke test passed."
    exit 0
fi

echo "QEMU smoke test failed: expected boot markers were not found."
[ -f "$log" ] && cat "$log"
exit 1
