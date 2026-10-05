#!/bin/sh
# usage: fetchrun.sh <smp> <wait> <fetch args...> : boots ICDA, runs /bin/fetch -s in the Terminal, prints [fetch] lines
smp=$1; wait=$2; shift 2
cd "$(dirname "$0")/.."
powershell -NoProfile -ExecutionPolicy Bypass -File .verify/boot.ps1 -Disk .verify/pw.img -Wait 35 -Smp $smp >/dev/null
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/qmp-input.ps1 -Move "61,150" -Click -Double -SettleMs 4000 >/dev/null
MSYS_NO_PATHCONV=1 timeout 200 powershell -NoProfile -ExecutionPolicy Bypass -File .verify/type.ps1 -Text "/bin/fetch -s $*" -Enter -SettleMs 40
sleep $wait
grep -a "^\[fetch\]" .verify/serial.log
