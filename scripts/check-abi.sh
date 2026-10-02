




set -eu
cd "$(dirname "$0")/.."

kernel_list="$(mktemp)"
user_list="$(mktemp)"
trap 'rm -f "$kernel_list" "$user_list"' EXIT INT TERM



sed -n 's/^[[:space:]]*\(SYS_[A-Z0-9_]*\)[[:space:]]*=[[:space:]]*\([0-9][0-9]*\),\{0,1\}.*$/\1=\2/p' \
    kernel/syscall/syscall.h | sort > "$kernel_list"


grep -o 'SYS_[A-Z0-9_]*' userspace/icda_sys.h | grep -vx 'SYS_H' | sort -u > "$user_list"

fail=0

while IFS='=' read -r name num; do
    if ! grep -qx "$name" "$user_list"; then
        echo "check-abi: $name (=$num) missing from userspace/icda_sys.h"
        fail=1
    fi
done < "$kernel_list"

while IFS= read -r name; do
    if ! grep -q "^${name}=" "$kernel_list"; then
        echo "check-abi: $name used by userspace but absent from kernel/syscall/syscall.h"
        fail=1
    fi
done < "$user_list"

count="$(wc -l < "$kernel_list" | tr -d ' ')"
if [ "$count" != "70" ]; then
    echo "check-abi: expected 70 native calls, found $count (ABI freeze violated?)"
    fail=1
fi

if [ "$fail" != "0" ]; then
    exit 1
fi
echo "check-abi: native ABI v1 OK ($count calls, kernel/userspace in sync)"
