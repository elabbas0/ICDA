#!/bin/sh
# Diagnostic: does QMP input reach the guest at all?
# Ctrl+Alt+F2 should switch to the text-shell VT (very visible).
rm -f /workspace/d1.ppm /workspace/wm-serial.log
( sleep 13
  echo '{"execute":"qmp_capabilities"}'
  sleep 0.5
  # hold ctrl+alt, tap f2
  echo '{"execute":"input-send-event","data":{"events":[{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"ctrl"}}}]}}'
  echo '{"execute":"input-send-event","data":{"events":[{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"alt"}}}]}}'
  echo '{"execute":"input-send-event","data":{"events":[{"type":"key","data":{"down":true,"key":{"type":"qcode","data":"f2"}}}]}}'
  echo '{"execute":"input-send-event","data":{"events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"f2"}}}]}}'
  echo '{"execute":"input-send-event","data":{"events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"alt"}}}]}}'
  echo '{"execute":"input-send-event","data":{"events":[{"type":"key","data":{"down":false,"key":{"type":"qcode","data":"ctrl"}}}]}}'
  sleep 1.5
  echo '{"execute":"human-monitor-command","arguments":{"command-line":"screendump /workspace/d1.ppm"}}'
  sleep 1
  echo '{"execute":"quit"}' ) | qemu-system-x86_64 -cdrom kernel.iso -m 256M \
    -vga std -vnc 127.0.0.1:1 \
    -no-reboot -qmp stdio \
    -serial file:/workspace/wm-serial.log >/dev/null 2>&1 || true
ls -la /workspace/d1.ppm 2>/dev/null || echo "NO DUMP"
