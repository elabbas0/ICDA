#!/bin/sh
# usage: tests/netem/run.sh <iso> <bytes> <wait> "<netem args>"
# ICDA in QEMU downloads from a real Linux TCP stack (SACK, CUBIC) in a
# container, through a QEMU socket <-> TAP bridge; netem shapes the link
# ("delay 40ms rate 20mbit loss 1%"). Build the image first:
#   docker build -t icda-nettest tests/netem
# A kernel built with NET_TEST_CFLAGS=-DNET_LOSS_TEST also drops bursts of
# received frames, the way a Wi-Fi receive ring that overflowed does.
iso=$1; bytes=$2; wait=$3; netem=$4
cd "$(dirname "$0")/../.."
docker rm -f icda-net >/dev/null 2>&1
MSYS_NO_PATHCONV=1 docker run -d --name icda-net --cap-add NET_ADMIN --device /dev/net/tun -p 127.0.0.1:9999:9999 \
    -e NETEM="$netem" -v "$(pwd -W)/tests/netem:/work" icda-nettest sh /work/server.sh >/dev/null
i=0; until docker logs icda-net 2>&1 | grep -q 'bridge ready'; do sleep 1; i=$((i+1)); [ $i -gt 60 ] && { docker logs icda-net; exit 1; }; done
powershell -NoProfile -ExecutionPolicy Bypass -File tests/netem/boot-sock.ps1 -Iso "$iso" -Disk ${DISK:-.verify/disk.img} -Wait 40 -Smp 2 -Accel whpx >/dev/null
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/qmp-input.ps1 -Key "ctrl+alt+t" -SettleMs 5000 >/dev/null
MSYS_NO_PATHCONV=1 timeout 200 powershell -NoProfile -ExecutionPolicy Bypass -File .verify/type.ps1 -Text "xxxx" -Enter -SettleMs 60 >/dev/null
MSYS_NO_PATHCONV=1 timeout 200 powershell -NoProfile -ExecutionPolicy Bypass -File .verify/type.ps1 -Text "/bin/fetch -s http://10.9.0.1:8099/$bytes" -Enter -SettleMs 60 >/dev/null
i=0; while [ $i -lt $wait ] && ! grep -a -q '^\[fetch\]' .verify/serial.log; do sleep 2; i=$((i+2)); done
grep -a '^\[fetch\]' .verify/serial.log || echo "[fetch] none after ${wait}s"
powershell -NoProfile -Command "Get-Process qemu-system-x86_64 -EA SilentlyContinue | Stop-Process -Force"
docker logs icda-net 2>&1 | grep -E 'sack|served|netem|SYN' | head -4
docker rm -f icda-net >/dev/null 2>&1
