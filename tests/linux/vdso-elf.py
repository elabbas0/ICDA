#!/usr/bin/env python3
"""Check the embedded vDSO agrees with its shared, read-only page mapping."""
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1] if len(sys.argv) > 1 else
            "kernel/linux/vdso/vdso.so").read_bytes()
assert data[:7] == b"\x7fELF\x02\x01\x01"
assert len(data) <= 4096, "vDSO must fit the single page copied by lx_vdso_map"
offset = struct.unpack_from("<Q", data, 32)[0]
entry_size, count = struct.unpack_from("<HH", data, 54)
headers = [struct.unpack_from("<IIQQQQQQ", data, offset + i * entry_size)
           for i in range(count)]
loads = [h for h in headers if h[0] == 1]
dynamic = [h for h in headers if h[0] == 2]
assert len(loads) == len(dynamic) == 1
load, dyn = loads[0], dynamic[0]
assert load[1] == 5, "PT_LOAD must advertise read/execute, not writable memory"
assert dyn[1] == 4, "writable PT_DYNAMIC makes glibc modify the read-only vDSO"
assert load[2] == load[3] == 0, "vDSO is copied at its ELF header"
assert load[5] == load[6] <= 4096
assert dyn[2] == dyn[3] and dyn[2] + dyn[5] <= load[5]
print("vDSO ELF: shared read-only mapping and loader metadata agree")
