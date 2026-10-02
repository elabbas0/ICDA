#!/usr/bin/env sh
# scripts/check-abi.sh — fail when the native syscall numbers drift
# between kernel/syscall/syscall.h and userspace/icda_sys.h.
# The ABI is frozen (see kernel/syscall/native_abi.h); run this in CI
# and before pushing any syscall-layer change.
set -eu
cd "$(dirname "$0")/.."

kernel_list="$(mktemp)"
user_list="$(mktemp)"
trap 'rm -f "$kernel_list" "$user_list"' EXIT INT TERM

# Strict enum parse: lines like "    SYS_FOO       = 12," (trailing comma
# optional, so the last entry matches too).
sed -n 's/^[[:space:]]*\(SYS_[A-Z0-9_]*\)[[:space:]]*=[[:space:]]*\([0-9][0-9]*\),\{0,1\}.*$/\1=\2/p' \
    kernel/syscall/syscall.h | sort > "$kernel_list"
# Userspace mirrors via `SYS_FOO` tokens in the sys_call wrappers
# (excluding the USERSPACE_ICDA_SYS_H include guard).
grep -o 'SYS_[A-Z0-9_]*' userspace/icda_sys.h | grep -vx 'SYS_H' | sort -u > "$user_list"

fail=0
# Every kernel number must appear in userspace...
while IFS='=' read -r name num; do
    if ! grep -qx "$name" "$user_list"; then
        echo "check-abi: $name (=$num) missing from userspace/icda_sys.h"
        fail=1
    fi
done < "$kernel_list"
# ...and userspace must not reference unknown numbers.
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
