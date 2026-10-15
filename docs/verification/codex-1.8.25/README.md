# Codex CLI verification — ICDA 1.8.25

Tested on 15 October 2026 in QEMU WHPX with two CPUs, 3 GiB RAM,
and an e1000 network adapter. This used the slim installed kernel and a
separate FAT32 ICDA system disk containing SYSTEM and the official Linux
runtime, rather than the full live ISO initramfs. This is VM evidence;
physical hotspot performance was not tested in this run.

## Reproduction and fixes

An older installed `SYSTEM/apps/shell.app` overrode the kernel's newer
fallback shell. Running `codex --version` then interpreted the POSIX launcher
with ICDA's native shell, producing unknown-command errors for `export`,
`for`, and `exec`. [Before screenshot](before.png).

The update ships the current native shell and explicitly invokes BusyBox sh
in the launcher shebang. The native loader preserves shebang interpreter
arguments in caller-owned storage. Linux helper initialization now follows
loading BusyBox from SYSTEM, so `/bin/sh` exists on installed boots too.
The active ICDA system volume is writable; foreign system volumes and EFI
remain protected. Codex uses foreground operation with shell snapshots
disabled for this runtime.

## Results

- `codex --version`: `codex-cli 0.162.1`, exit 0.
- `codex login status`: `Logged in using ChatGPT`, exit 0.
- `run /bin/codex --version`: `codex-cli 0.162.1`, exit 0.
- An interactive request executed `/bin/busybox echo ICDA_SHELL_1825_OK`
  and returned `ICDA_SHELL_1825_OK`.
- Codex searched the web for official CLI documentation and opened
  `https://developers.openai.com/codex/cli/`.
  [Web-search screenshot](web-search.png).
- Codex used BusyBox to write and read
  `/volumes/fat32-0/SYSTEM/CODEXPRF.TXT`, returning
  `ICDA_CODEX_1825_TOOLS_VERIFIED`.
  [System-write screenshot](system-write.png).
  An independent mtools read from the disk image returned the same text.

The tested disk's kernel and launcher hashes match the signed 1.8.25 OTA:

```text
EFI/ICDA/KERNEL.BIN
b98325c6aaa3a8349ffff11e3bddac0b09c7a042e2b65a2383bb14be8848592b
linux/bin/codex
c77778513f488998853b2f9a6b4684cf7a6b348691f64bb19fe7e0a90d03fa97
```

The CLI still reports a PATH-alias creation warning (`Operation not
permitted`). The successful shell, network, and file tests above do not
establish support for every CLI feature or every Linux syscall.

## Build and focused regression checks

The kernel, installed kernel, and OTA build completed successfully.
The two regression programs can be run in the `icda-toolchain` container:

```sh
gcc -ffunction-sections -fdata-sections tests/user-shebang.c \
  -Wl,--gc-sections -o /tmp/icda-user-shebang
/tmp/icda-user-shebang
gcc -ffunction-sections -fdata-sections tests/volume-write-policy.c \
  -Wl,--gc-sections -o /tmp/icda-volume-policy
/tmp/icda-volume-policy
```

Both passed: interpreter arguments remain valid after parsing; only the
active writable ICDA system partition receives system-volume write access.
The test authentication and raw session files are private and are not
included in the source or OTA.
