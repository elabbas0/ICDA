# ICDA native syscall ABI, version 1 (frozen)

The native numbers `SYS_CONSOLE_WRITE` (0) through `SYS_PROC_STATS` (69)
are a stable kernel/userspace contract. `userspace/icda_sys.h` mirrors
every number; `scripts/check-abi.sh` enforces the sync (70 calls).

Rules for changing anything in `kernel/syscall/syscall.c` dispatch:

- Do NOT renumber, remove, or repurpose an existing number.
- New capability goes to a userspace server (netd/vfsd/audiod) behind
  the existing IPC calls (`SYS_MSG_*`, `SYS_SHM_*`), not to a new
  `SYS_*` number. If a number is ever truly unavoidable, bump
  `ICDA_NATIVE_ABI_VERSION` in `kernel/syscall/native_abi.h` and
  document the addition here.
- The Linux personality (`linux_syscall_dispatch`, Linux numbers) is a
  separate namespace and is NOT covered by this freeze; it grows toward
  musl compat instead.

Call table (number — name — rough area):

| nr | name | area |
|----|------|------|
| 0 | SYS_CONSOLE_WRITE | console (/dev/console) |
| 1 | SYS_GET_PID | proc |
| 2 | SYS_VFS_READ | vfs |
| 3 | SYS_VFS_WRITE | vfs |
| 4 | SYS_EXIT | proc |
| 5 | SYS_INPUT_READ | input (/dev/input) |
| 6 | SYS_GETCWD | vfs |
| 7 | SYS_CHDIR | vfs |
| 8 | SYS_LIST_DIR | vfs |
| 9 | SYS_EXEC | proc |
| 10 | SYS_CONSOLE_CLEAR | console (/dev/console) |
| 11 | SYS_CONSOLE_BACKSPACE | console (/dev/console) |
| 12 | SYS_MKDIR | vfs |
| 13 | SYS_CREATE | vfs |
| 14 | SYS_STAT | vfs |
| 15 | SYS_LIST_PROCS | proc |
| 16 | SYS_SPAWN | proc |
| 17 | SYS_WAITPID | proc |
| 18 | SYS_YIELD | sched |
| 19 | SYS_SLEEP | sched |
| 20 | SYS_PROC_INFO | proc |
| 21 | SYS_KILL | proc |
| 22 | SYS_SUSPEND | proc |
| 23 | SYS_RESUME | proc |
| 24 | SYS_INPUT_READLINE | input (/dev/input) |
| 25 | SYS_SYNC | vfs |
| 26 | SYS_CONSOLE_SETCURSOR | console (/dev/console) |
| 27 | SYS_STORAGE_INFO | storage |
| 28 | SYS_SOUND_PLAY | audio |
| 29 | SYS_AUDIO_PCM_PLAY | audio |
| 30 | SYS_AUDIO_PLAY_FILE | audio |
| 31 | SYS_AUDIO_STOP | audio |
| 32 | SYS_AUDIO_STATUS | audio |
| 33 | SYS_AUDIO_CLAIM | audio |
| 34 | SYS_AUDIO_READ_CHUNK | audio |
| 35 | SYS_AUDIO_FINISH | audio |
| 36 | SYS_TICKS | sched |
| 37 | SYS_INPUT_READ_TIMEOUT | input (/dev/input) |
| 38 | SYS_VFS_READ_AT | vfs |
| 39 | SYS_INSTALL_SYSTEM | install |
| 40 | SYS_MOUNT | vfs |
| 41 | SYS_FORMAT_DEVICE | storage |
| 42 | SYS_CONSOLE_SIZE | console (/dev/fb0) |
| 43 | SYS_INSTALL_DEVICE | install |
| 44 | SYS_RUNTIME_DEVICE | storage |
| 45 | SYS_INSTALL_PARTITIONS | install |
| 46 | SYS_FORMAT_PARTITION | storage |
| 47 | SYS_SET_PARTITION_ROLE | storage |
| 48 | SYS_HTTP_GET_IPV4 | net (server-bound) |
| 49 | SYS_CONSOLE_GETCURSOR | console (/dev/console) |
| 50 | SYS_DNS_RESOLVE | net (server-bound) |
| 51 | SYS_HTTPS_GET_IPV4 | net (server-bound) |
| 52 | SYS_EXEC_ARGS | proc |
| 53 | SYS_SPAWN_ARGS | proc |
| 54 | SYS_SHM_CREATE | ipc (core, stays) |
| 55 | SYS_SHM_MAP | ipc (core, stays) |
| 56 | SYS_SHM_UNMAP | ipc (core, stays) |
| 57 | SYS_SHM_CLOSE | ipc (core, stays) |
| 58 | SYS_MSG_OPEN | ipc (core, stays) |
| 59 | SYS_MSG_SEND | ipc (core, stays) |
| 60 | SYS_MSG_RECV | ipc (core, stays) |
| 61 | SYS_MSG_POLL | ipc (core, stays) |
| 62 | SYS_MAP_FRAMEBUFFER | display (/dev/fb0) |
| 63 | SYS_INPUT_READ_MOUSE | input (direct, mouse bypasses /dev for now) |
| 64 | SYS_GUI_AVAILABLE | display (/dev/fb0) |
| 65 | SYS_GPU_QUERY | display (/dev/fb0) |
| 66 | SYS_GPU_PRESENT | display (/dev/fb0) |
| 67 | SYS_GPU_CURSOR | display (/dev/fb0) |
| 68 | SYS_POWER | power |
| 69 | SYS_PROC_STATS | proc |

`/dev` nodes (all `VFS_NODE_FILE`, discoverable via `ls /dev`):
`/dev/console`, `/dev/input`, `/dev/fb0`. Dispatch goes through the
`devops` registry (`kernel/dev/devops.c`); the VFS nodes mirror it for
discoverability. Server-bound calls (net/install/audio/…​) keep their
numbers but their implementations must migrate to userspace servers;
the numbers then become thin IPC proxies, never removed.
