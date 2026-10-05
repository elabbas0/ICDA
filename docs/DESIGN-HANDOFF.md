# Design overhaul: status

Branch: `dev`. The original tree arrived with no git history, so the
first commits bundle the import with work already done to it; see the
commit bodies for exactly what was imported versus rewritten.

Current state:

- **Builds clean.** A clean `make` exits 0 with zero compiler warnings
  in the kernel and userspace (only the linker's build-id note remains),
  and `scripts/check-abi.sh` reports the 70 native calls in sync. Getting
  there removed dead code left by the v1.5.0 cleanup (the kernel shell's
  old job control and line editor, unused HDA amp helpers, `zero_bytes`
  in fat32) and switched the console, input and fb `dev_calls_t` tables
  to designated initialisers.
- **The comment-strip pass was audited.** Comparing every C file before
  and after it with comments removed (`gcc -fpreprocessed -E`) showed one
  real change outside the intended legacy-UI removal: the Browser's
  protocol-relative link prefix had become `"https://"`, so `//host/x`
  resolved to `https:////host/x`. Restored. Assembly, shell scripts and
  the Makefile only lost comments; the three PowerShell scripts that lost
  `<# #>` blocks were repaired and all scripts now parse.
- **Boots.** Verified under QEMU: the desktop renders (wallpaper, icons,
  taskbar with live clock, window frames).
- **Smoke tests pass**, but they prove only that the kernel reaches its
  boot markers. The kernel disables the serial mirror once the
  framebuffer is up, so headless logs always stop at `[S22 shell]`
  whether or not the GUI started. Use `scripts/qmp-screendump.ps1` to
  actually look at the desktop.

## Verification status per app

The apps were migrated and each builds warning-free, but compiling is
not evidence that an app runs. Driving them needs a headless QEMU with a
QMP port, then screenshots:

```sh
# start the VM headless, then capture
qemu-system-x86_64 -machine q35 -smp 4 -m 4G -cdrom kernel.iso -boot d \
    -display none -monitor none -serial file:.verify/serial.log \
    -qmp tcp:127.0.0.1:4444,server,nowait -no-reboot
powershell -File scripts/qmp-input.ps1 -Port 4444 -Move "63,150" -Double
powershell -File scripts/qmp-screendump.ps1 -Port 4444 -Out .verify/app.png
```

| App | File | Shell | Verified running |
|---|---|---|---|
| Terminal | `terminal.c` | Console | **yes** |
| Explorer | `desktop.c` | Toolbar + sidebar + grid/list | **yes** |
| Music | `audioplay.c` | List + now-playing | **yes** |
| Activity | `taskman.c` | Monitor | **yes** |
| Editor | `editor.c` | Toolbar + text + status | **yes** |
| Disk Utility | `diskman.c` | Toolbar + sidebar + detail | **yes** |
| Browser | `browser.c` | Toolbar + address + page | **yes** |
| ICDA Demo | `gui_demo.c` | Control gallery (CI image) | **yes** |

What the verified apps cover:

- Terminal: window opens, heading/hint/prompt render in the right roles,
  caret visible, scrollback reachable.
- Explorer: sidebar navigation to `/apps` lists all 14 `.app` files, the
  status bar reports `/apps  14 items`, both the icon grid and the list
  view render, and the toolbar buttons respond.
- Music: `.wav` discovery, track list, selection pill, disabled Stop
  while idle, now-playing bar, and correct reflow when the window is
  maximized (so `IC_EV_RESIZE` is handled).
- Activity: process table with named kernel tasks and readable states,
  row selection enables Suspend/Quit, Suspend confirm, Suspended state and
  Resume label, storage summary in the footer.
- Editor: typing, Enter, Tab, arrow keys, gutter numbers, status bar
  (Ln/Col, bytes), Save, and the unsaved-changes guard on New.
- Disk Utility (with a 2 GB scratch disk): device list, FAT32 erase,
  ICDA layout (EFI/Swap/System on GPT), partition selection, role change,
  Cancel/Continue in the confirm dialog.
- Browser (QEMU `-nic user,model=e1000`): Loading state, page fetch and
  render of `http://example.com`, error state when the network is down.

Bugs the second pass found and fixed: the AHCI driver allocated one page
too few for its DMA window, so any transfer of 24+ sectors overwrote the
next physical page (it erased the device name on a GPT write); setting a
partition role wrote GPT entry `index % 4`; Disk Utility never parsed the
`partitions:` section and only recognised devices whose name starts with
`a`; arrow keys reached apps as ESC + `[A` because a lone ESC was flushed
after one idle loop pass; the Editor drew past the end of a line after a
tab and spun forever on lines wider than the view; the Editor status bar
was placed at `y = width - 24`; `ic_ui_alert` was shorter than its own
content and apps hit-tested different button rects than it drew;
scrollbars were passed row counts where pixels were expected, so they
never appeared; the WM placed cascaded windows past the right edge.

Bugs the first pass found and fixed, none of which the compiler or the smoke test
could see: the Terminal prompt overwrote the hint line (row accounting
disagreed with `layout()`), the Explorer sidebar caption sat under the
first item, the Explorer toolbar buttons were drawn outside the client
area, and Explorer toolbar clicks selected grid cells (C truncates
negative integer division toward zero, so the bounds check never fired).

Harness notes: PS/2 motion is relative only, <=127 px per packet, and
one axis per `input-send-event` call, or QEMU drops the y event. The
first rel event after boot can be lost, so issue a warm-up move. A
double-click must keep both presses inside 400 ms (`DBLCLICK_TICKS` in
`wm.c` is 40 ticks at 100 Hz) or the WM reads two single clicks.

QMP pointer coordinates are **screen** coordinates; apps receive
**client** coordinates. Add the window origin (roughly +16, +46 for a
default-placed window) when aiming at a control inside a window.

A running QEMU holds `kernel.iso` open, so `make kernel.iso` fails with
`mv: Permission denied` at the end. Stop the VM before rebuilding.

The taskbar launcher is a more reliable target than the desktop icons:
one click opens the panel, one click launches.

## Terminal and the pty layer

The Terminal is a real terminal emulator now. The kernel has pseudo
terminals (`kernel/tty/pty.c`): `pty_open` gives the caller a master,
`pty_spawn` starts a program attached to it, and `pty_io` reads output,
writes input or resizes. A process attached to a pty, and every child it
spawns (the field is inherited), has its console calls redirected: writes
go to the pty, key reads come from it, `read_line` is cooked with echo,
clear/backspace/set-cursor become ANSI sequences, `console_size` reports
the pty size, and the cursor query fails so programs know they are on a
pty. Linux-personality `read`/`write` on stdio go through it too. Ctrl+C
(0x03 from the master) kills the newest process under the shell; closing
the Terminal kills everything attached.

The Terminal runs `/apps/shell.app` and emulates VT100/xterm: cursor
motion, erase, insert/delete, scroll regions, save/restore cursor, SGR
colours (16 plus the low 256 range), bold, underline and inverse, with
2000 lines of scrollback (wheel, Shift+PgUp/PgDn, scrollbar drag). The
shell redraws its line with save/restore cursor on a pty and colours its
prompt.

This bumped the native ABI to v2: 73 calls (`SYS_PTY_OPEN` 70,
`SYS_PTY_SPAWN` 71, `SYS_PTY_IO` 72). v1 numbers are unchanged.

## Editor

The Editor is laid out like VS Code: a folder tree on the left that you
can toggle (grid button or Ctrl+B), plus Open Folder, Open File, New and
Save in the toolbar.

- **Opening things.** Ctrl+O opens a file. Typing a directory into the
  file prompt opens it as a folder.
- **The tree.** Folders are listed first, sorted by name, and dotfiles
  are hidden. Clicking a folder expands or collapses it, and clicking a
  file opens it.
- **Unsaved changes.** An action that would drop unsaved changes asks
  once. Repeating it discards them.
- **Highlighting.** Syntax highlighting is chosen by file extension: C,
  JS/TS, Python, Shell, Makefile, ASM, JSON and Markdown. Colours come
  from `ic_syntax_color()` in `ic_theme.c` and have light and dark
  variants. The status bar shows the detected language.

## Userspace memory (native ABI v4)

Native apps now have a heap. `SYS_VM_ALLOC` (74) maps zeroed anonymous pages
and returns their address; `SYS_VM_FREE` (75) unmaps them. Both share
`uvm_map_anon` in `syscall.c` with the Linux `mmap` path. The region starts at
`0x70000000`, and `SYS_VM_FREE` only unmaps inside it, so it cannot release
program image or shared-memory pages. A failed map rolls back the pages it
already mapped.

`ic_mem.c` in libicda builds `ic_malloc`, `ic_calloc`, `ic_realloc` and
`ic_free` on top of those calls. Small blocks use boundary tags with coalescing
in 1 MB arenas. Blocks of 256 KB or more get their own mapping and go back to
the kernel on free. Apps are single-threaded, so there is no locking. The
Editor buffer now grows as needed (the 64 KB cap is gone; a 923 KB file opens),
and Explorer rename has no size limit.

## C library and SDK

`userspace/libc` is a small static C library for native programs:
- **Headers:** `string.h`, `stdlib.h`, `ctype.h` and `stdio.h`. `stdint.h`,
  `stddef.h` and `stdarg.h` come from GCC's freestanding headers.
- **printf family:** supports widths, precision, flags and the `l`/`ll`/`z`
  length modifiers.
- **Console:** `stdout` is line-buffered to the console or pty. `stdin` reads
  cooked lines through `SYS_INPUT_READLINE`.
- **Files:** `FILE*` streams load the whole file on `fopen` and write it back
  on `fflush`/`fclose`, because the VFS has whole-file read/write calls. Modes
  r, w, a and + work, and so do seeking and `remove`/`rename`.
- **Memory:** malloc maps onto `ic_mem`.
- **Startup:** `crt1.asm` calls `main` through `exit()`, so atexit handlers run
  and buffered output is flushed.
- **Build:** `libc.o` is `libc_core.o` plus `ic_mem.o`. It is compiled with
  `-fno-builtin -fno-tree-loop-distribute-patterns` so GCC does not turn
  `memset`/`memcpy` loops into calls to themselves.

Native vs Linux binaries: the loader runs ELFs under `/bin/` with the Linux
personality, except when byte 7 of the ELF header (`EI_OSABI`) is `0xFF`
(`USER_ELF_OSABI_ICDA`). The libc link step writes that byte.

`/bin/libctest.elf` is the regression test (run `libctest.elf` in the
Terminal; expected `32/32 passed`). It caught a real bug: `SYS_VFS_READ`
stores at most `cap - 1` bytes and a NUL, so a read sized to the file's exact
length drops the last byte. Pass `size + 1`.

`make sdk` writes `sdk/` (gitignored) with the headers, `crt1.o`, `libc.o`,
`user.ld`, a Makefile and `hello.c`. Running `make` there builds `hello.elf`
with the host gcc/ld. Copy the result onto a volume ICDA mounts, or fetch it
with curl, and run it by path.

## Disks, partitioning and dual boot (native ABI v5)

### FAT32 driver
`kernel/fs/fatfs.c` is the FAT32 read/write driver. It supports:
- **Paths:** lookup, read, write (create or replace), mkdir-p and recursive
  remove.
- **Names:** long file names with checksums, unique `~N` short aliases, and
  case-insensitive lookup.
- **Directories:** they grow when full.
- **FAT updates:** a write-back FAT sector cache mirrored to every FAT copy.
  FSInfo's free count is set to "unknown"; the spec allows that and Windows
  recounts.
- **Safe replace:** a new file is written and linked before the old clusters
  are freed.

The installer uses it. It used to create a second `EFI` directory next to an
existing one (which corrupted a shared ESP) and leaked the clusters of
replaced files.

### Writable FAT32 volumes
FAT32 partitions are mounted at boot (and again after any partition edit) at
`/volumes/fat32-N`, with long file names. Changes made through the VFS go
straight to disk:
- **VFS hook:** every node has a `mount_id`, inherited from its mount root.
  `vfs_mkdir`, `vfs_create`, `vfs_write`, `vfs_node_write_at` and `vfs_remove`
  on a mounted node call the external hook registered with
  `vfs_set_external_hook`. If the disk operation fails, the VFS operation
  fails.
- **Disk side:** `fat32.c` maps the path to the volume and runs
  `fatfs_mkdir`/`fatfs_write`/`fatfs_remove` on a fresh `fatfs` mount for
  each operation. Nothing is cached between operations, so the installer and
  persistence can safely write the same disks.
- **Read-only volumes:** EFI partitions and ICDA's own system partition are
  read-only. Mount roots are read-only, so a volume cannot be deleted.
- **Persistence:** `persistfs` skips anything with a `mount_id`, so volume
  contents never end up in ICDA's root bundle.

Lazy loading:
- **Mounting** imports only the directory tree. File nodes are `lazy`: they
  carry the size, but no data.
- **Ranged reads** (`vfs_node_read_at`, used by `SYS_VFS_READ_AT` and `fd`
  reads through a 64 KB bounce buffer) go straight to disk through the loader
  set with `vfs_set_loader`. `fat32.c` looks up the path for every read, so it
  stays coherent with other writers. It keeps a cluster hint (first cluster,
  offset, cluster), so sequential chunked reads don't re-walk the chain.
- **Whole-file access** (`vfs_read`, `vfs_node_data`, and partial writes,
  which need the old contents) loads the file once on first use and caches it.
  That is capped at `VFS_LAZY_LOAD_MAX` (256 MB); larger files are readable in
  ranges only.
- **Full rewrites** (`vfs_write`) replace the data without loading it first.

Limits: a partial write (`write_at`) still rewrites the whole file on disk, and
loaded files stay cached until the volume is remounted. The shell gained `rm`,
and `write "path with spaces" text`. Explorer, the Editor, libc `fopen` and the
shell all work on volumes through the normal VFS calls. `libctest` checks
ranged reads against `.verify/mkwin.sh`'s 40 MB pattern file when it is
present.

### Writable exFAT
`kernel/fs/exfatfs.c` is an exFAT read/write driver:
- **Mount** checks the boot region checksum and finds the allocation bitmap
  and upcase table in the root directory.
- **Allocation** uses the bitmap (one-sector write-back cache) and prefers a
  contiguous run with NoFatChain, as Windows does. Otherwise it falls back to a
  FAT chain (one-sector FAT cache).
- **Entry sets** (file, stream extension, names) are written with the spec's
  set checksum and a name hash over the volume's upcase table. The table is
  loaded once per mount; the compressed format is supported.
- **Directories** grow when full. A NoFatChain directory that cannot grow in
  place is converted to a FAT chain, and its size in the parent's entry set is
  updated.
- **Reads** are ranged and respect ValidDataLength, with the same cluster hint
  as FAT32. Write (replace), mkdir-p and recursive remove are supported.
- **Read-only cases:** volumes with two FATs (TexFAT) or a fragmented bitmap
  mount read-only.

`kernel/fs/volumes.c` now owns the mount table, the VFS write-through hook and
the lazy loader for both filesystems, and dispatches by mount id.
`fat32.c`/`exfat.c` are thin wrappers. exFAT volumes mount at
`/volumes/exfat-N`, and storage info marks only the truly read-only mounts
with `(ro)`. Format, create, delete and resize all remount the volumes.

The exFAT formatter (`diskfmt_format_exfat_partition`) was rewritten. The old
one wrote a single boot sector, which no OS would mount. It now writes:
- main and backup boot regions with checksum sectors;
- a FAT sized for the cluster count;
- the allocation bitmap;
- the standard compressed upcase table from the specification
  (`exfat_upcase.h`, 5836 bytes, checksum 0xE619D30D);
- a root directory with label, bitmap and upcase entries.

Clusters are 4 KB up to 256 MB, 32 KB up to 32 GB, then 128 KB.

Verification uses a separate `icda-verify` Docker image (Debian with
exfatprogs, gdisk, mtools, dosfstools and gcc). A privileged container
loop-mounts images with the Linux exFAT driver:
- `.verify/mkex.sh` builds a disk with an exFAT volume made by `mkfs.exfat`
  and filled through Linux.
- `.verify/checkex.sh` runs `fsck.exfat` and reads the files back with Linux.

`libctest` adds a 200-file create/read/delete test on `/volumes/exfat-0` when
present, which also forces directory growth.

### Read-only NTFS
`kernel/fs/ntfsfs.c` reads NTFS:
- **MFT records** are read through the `$MFT` data runs, with update-sequence
  fixups.
- **Data runs** support sparse runs. Data beyond the initialized size reads
  as zeros. `$ATTRIBUTE_LIST` is followed, so fragmented files and large
  directories work.
- **Directories** are listed from `$INDEX_ROOT` plus every `INDX` block of
  `$INDEX_ALLOCATION`. DOS short-name duplicates and `$` system files are
  skipped.
- **Small files** stored inside their MFT record (resident `$DATA`) are
  copied directly.
- **Compressed and encrypted files** report a read error instead of returning
  garbage.

NTFS volumes mount read-only at `/volumes/ntfs-N` through `volumes.c`. A
Windows system drive has far too many files to import up front, so the VFS
gained lazy directories:
- A node marked `lazy_dir` is populated through `vfs_set_dir_loader` the
  first time a child is looked up or the directory is listed. The flag is
  cleared before the hook runs, so the imports don't recurse.
- Every lazy node carries an `ext_ref` (the MFT record number), so loads go
  straight to the record without path lookups. The loader hook now receives
  that `ref`.

Limits: names longer than 63 characters (the VFS name limit) are skipped, and
sizes come from the directory index's `$FILE_NAME` copy.

`.verify/mkntfs.sh` builds a test disk with `ntfs-3g` (in `icda-verify`): the
40 MB pattern file, a tiny resident file, nested folders with spaces, and a
3,000-file folder. `libctest`'s range test also covers `/volumes/ntfs-0`.

### Installing next to other systems
- **Boot files:** everything goes in `\EFI\ICDA\` (`GRUBX64.EFI`,
  `KERNEL.BIN`). `\EFI\BOOT\BOOTX64.EFI` and `STARTUP.NSH` are written only
  when absent, so another OS's loader is never replaced. An ESP that is
  already typed EFI is not renamed.
- **Firmware boot entry:** `kernel/firmware/efi.c` registers a UEFI boot
  entry after install, as `efibootmgr` does.
  - It reads the EFI system table and memory map from multiboot2 tags 12 and
    17.
  - It identity-maps only `EFI_MEMORY_RUNTIME` regions, and only pages not
    already mapped, uncached for MMIO.
  - It calls `GetVariable`/`SetVariable` with the MS ABI and interrupts off.
  - It writes `Boot####` "ICDA", reusing an existing entry with the same
    label, as a hard-drive device path from the ESP's GPT unique GUID plus
    `\EFI\ICDA\GRUBX64.EFI`, and puts it first in `BootOrder`.
- **GRUB menu:** the installed config (`grub-install.cfg`) finds Windows
  (`/EFI/Microsoft/Boot/bootmgfw.efi`) and Ubuntu (`/EFI/ubuntu/shimx64.efi`)
  on any ESP. When it finds one it shows a 5-second menu with chainload
  entries and UEFI Firmware Settings.

### Partition types and roles
ICDA's own partition has its own GPT type
(`5E2A3F8C-1D4B-4E6A-9C7D-1CDA00000001`). It used to share Microsoft Basic
Data, which made Windows partitions look like ICDA ones. Old installs are still
recognised by the name "ICDA System". Roles now include data, msr, recovery and
linux, so tools never mistake Windows data for ICDA.

### Partition editing (`SYS_DISK_EDIT` = 76)
- **Free space:** `diskfmt_free_regions` lists 1 MiB-aligned gaps (GPT usable
  range or MBR). Storage info prints them under `free:` together with
  `firmware=uefi|bios`.
- **Operations:** create (only inside a free gap, with a random unique GUID
  and optional FAT32/exFAT format), delete, resize, FAT32 usage, firmware
  type, and install status.
- **Resize** is FAT32 only. It keeps at least 65525 clusters, keeps every
  used cluster, stays within the existing FAT, and updates both boot sectors.
  NTFS is never resized; the UI points to Windows Disk Management instead.
- **Safety:** every edit verifies the GPT header and entry CRCs first,
  rewrites primary and backup tables, and refuses the disk the system runs
  from. `sgdisk -v` reports no problems after create, resize and delete.

### Disk Utility
`userspace/diskman.c` is laid out like GNOME Disks:
- **Drive list** with drawn drive icons (`ic_volume_color` holds the colours).
- **Volumes map** with colours per partition type, hatched free space and
  usage bars.
- **Actions:** + (create), - (delete) and a gear menu (format, resize,
  change type).
- **Details:** size, contents, type, disk and location.

**Install ICDA...** first asks **Automatic** or **Manual**:
- **Automatic:** install alongside other systems in the largest free gap
  (adding a 260 MB ESP only if none exists), or erase the disk and use the
  ICDA layout.
- **Manual:** pick the EFI and ICDA partitions from this disk's FAT32 volumes,
  optionally formatting the ICDA partition.

The install runs in a child `diskman.app --install E R` /
`--install-device D`. The window polls `SYS_DISK_EDIT` op 6 for a progress
bar. While the GUI owns the screen the kernel no longer draws its text-mode
progress over the desktop, and `console_clear` respects the framebuffer mute.

### Other fixes
- **AHCI timeouts** are time-based (30 s, using `sched_ticks`, with an
  iteration fallback before the timer starts). Fixed spin counts failed on
  slow writes.
- **New FAT32 volumes** get a random serial and the "NO NAME" label.

### Testing
- `.verify/mkwin.sh` builds a Windows-like GPT disk: an ESP with fake Windows
  loaders, a basic-data partition, a FAT32 partition and free space.
- `.verify/checkwin.sh` runs fsck and compares the files.
- `.verify/boot-uefi.ps1` boots under OVMF with a writable variable store.
- `.verify/dualboot-test.ps1` runs install, reboot and the GRUB menu end to
  end.

## Boot animation

Startup mirrors the shutdown animation (which fades the desktop to black and
shows an icon and a caption):
- **Kernel splash** (`kernel/diag/splash.c`): pure black, a centred "ICDA"
  wordmark and a thin rounded progress bar. It is drawn with integer 4x4
  subpixel anti-aliasing, because the kernel builds without SSE. The wordmark
  is an alpha mask rendered from Inter Display Bold by
  `scripts/gen_boot_logo.py` into `kernel/diag/boot_logo.h`.
  `kernel/diag/boot_layout.h` holds the shared geometry and the auto-2x rule.
- **Handoff:** when the GUI is about to start, the kernel leaves the finished
  splash (bar at 100%) on screen instead of clearing it.
- **WM intro:** the WM's first frames (`wm_intro_sequence` in `wm.c`,
  `wm_boot_overlay_draw` in `wm_shell.c`) redraw the identical splash from the
  same mask. Over 0.9 s the wordmark and bar fade out and the desktop fades in
  from black. It respects the existing boot-animation setting.
- **Stage stamps:** `bootstage_set` no longer draws `[S22 shell]`-style
  stamps on screen; stage names go to the serial log only.

`.verify/burst.ps1` takes rapid screendumps over one QMP connection, which is
how the fade was checked frame by frame.

## Explorer delete (native ABI v3)

The VFS now has `vfs_remove`, exposed as `SYS_VFS_REMOVE` (73), which
bumps the native ABI to v3 with 74 calls (`scripts/check-abi.sh` checks
this). It unlinks a file or a whole folder from the tree and persists the
change. It refuses the root, anything read-only (seeded system files), and
the caller's own working directory or its parents. Removed nodes are
detached but not freed, so open fds and other processes' cwd pointers
never dangle; the small leak is the price of that.

In Explorer, the context menu has Delete and the Delete key works. Both
ask for confirmation first. Rename now writes the new name and removes
the old one (any size since ABI v4; folders still cannot be renamed). There
was also a dialog bug that cleared the typed name before it was used,
which broke New Folder, New File, Go to Folder and Rename. That is fixed.

## Remaining work

### P1: remove the legacy UI API

Done. The legacy `libicda` UI API has been removed; code comments were moved to
`docs/code-comments.md`. Color hex literals in `userspace/*.c` now live only
in `ic_theme.c` and `wm_shell.c` (wallpaper glows). Everything else uses
`IC_WHITE`/`IC_BLACK`/`IC_*_A()` (`ic_gfx.h`) or `IC_TINT_*` (`ic_theme.h`).
The hex values left in `libicda.c` and `nptestlx.c` are not colors.

### P2: verification

Done.
- All eight apps are verified running (table above). The ICDA Demo only
  exists in the CI image (`make kernel.iso CI_IMAGE=1`); start it from the
  Terminal with `gui_demo.app`.
- `scripts/run-selftest-gates.sh` passes both gates (nptest, nptestlx:
  `NPTEST DONE ALL-PASS`). The script now fails a gate when its build
  fails instead of booting a stale image. Do not run it while a QEMU holds
  `kernel.iso` open.
- WM paths exercised by screenshot: maximize, restore, minimize and
  restore from the taskbar, close, edge resize (the app reflows), desktop
  context menu ("Add Editor"), icon context menu and Get Info, and the
  shutdown/restart overlay (QEMU powers off). Both 1024x768 and 1920x1080
  layouts render correctly.
- Shm lifetime: the kernel now releases a process's shm mappings when it
  exits (`shm_proc_exit`, called next to every `fd_proc_exit`), so a
  region an app never adopted or closed no longer leaks when the app dies.
  Regions still in flight to a live app that never processes RESIZE are
  only reclaimed when that app exits.

Bugs this pass found and fixed: caption hover stayed on a button after a
click until the pointer moved; the launcher, context menu and Get Info
left a ghost of their last fade frame because nothing repainted after the
fade ended; the Demo laid its sections out under the header with labels
inside the previous group, never applied the switch/segmented tweens,
hit-tested buttons by guessed x ranges, and printed 40% as "04%".

Testing other resolutions: GRUB's `gfxmode` is ignored because the kernel's
multiboot2 framebuffer tag decides the mode (width/height 0 gives GRUB's
default, 800x600x24 under QEMU). Variant builds
patch lines 30-31 of `kernel/boot.asm` to request a fixed size;
`scripts/build-resolution-iso.sh 1024x768 1920x1080` does that.

Harness additions: `scripts/qmp-input.ps1` takes `-Key shift+minus`
chords, `-Wheel N`, and `-Press`/`-Release` for drags. `ppm2png.ps1`
used to skip every whitespace-valued byte after the PPM maxval, so a frame
whose first pixel was dark (bytes 9, 10, 13 or 32) came out shifted by a
byte or two, which looks exactly like rotated colour channels. It now
skips exactly one byte, as P6 requires. Distrust any older screenshot of
a dark full-screen overlay.

### P3: polish
- Cursor: done. The WM builds 4x4-supersampled sprites with a dark rim
  and a blurred drop shadow at startup (`build_cursor_sprite`), each with
  its own hotspot: arrow, and east-west, north-south and both diagonal
  resize shapes chosen from the frame hit under the pointer, plus an
  I-beam. Apps request it with `ic_app_set_cursor(app, IC_CURSOR_TEXT)`
  (sent as `GUI_MSG_SET_CURSOR` only on change); the WM shows it while
  the pointer is over that window's client area. Editor, Terminal, the
  Browser address bar and the Demo text field use it.
- Blur cost: done differently. `ic_gfx_backdrop` now blurs only the clip
  rectangle plus the blur's reach (3 box passes of radius blur/2), which
  is pixel-identical inside the clip (checked against a full blur on the
  host), and the WM grows a dirty rect into the taskbar or a panel only by
  that reach instead of to the whole material. Updates near the taskbar
  no longer re-blur all of it.
- Composite cost: done. Dirty rects are coalesced until none overlap, so
  no region is composited twice, and when they cover more than 60% of
  the screen the frame is composited once as a whole.
- HiDPI: partly done. `scale=` in the settings file (0 auto, 1, 2; auto
  picks 2 at 2560 px wide and up). The WM composes at logical resolution
  and pixel-doubles in `blit_region`, and divides pointer coordinates, so
  everything is correctly sized on 2560x1440 and 4K (which previously
  overflowed the 2560x1600 back buffer). Text and icons are doubled, not
  re-rasterised: true 2x needs a scale in `ic_canvas_t` and 2x font and
  icon atlases from `gen_fonts.py`/`gen_icons.py`. Verified at 2560x1440.
- Mouse wheel: done. The PS/2 driver does the IntelliMouse handshake
  (rates 200/100/80, then ID 3 means 4-byte packets) and reports `dz`.
  `dz` is appended to `mouse_event_t`, `syscall_mouse_event_t` and
  `icda_mouse_event_t`; it fits in existing padding, so the struct size
  is unchanged. The WM forwards it in `gui_msg_t.mouse.wheel` to the
  window under the pointer, and `ic_app` emits `IC_EV_SCROLL` with
  `ev->wheel` (positive scrolls down). Terminal, Editor, Explorer, Music,
  Activity, Browser and the Demo scroll 3 rows per notch. `ic_app` adds
  inertia: notches under 120 ms apart build velocity, and after a fast
  flick it keeps emitting decaying scroll steps.
  Wiring this up exposed that Terminal Page Up/Down and scrollbar drag
  were inverted, and Explorer paging never worked (grid layout reset the
  scroll every frame; list view ignored the keys); both are fixed. Lists
  now follow the selection only when it changes, so wheel scrolling sticks.
- Keyboard: done. The PS/2 driver emits xterm-style sequences with a
  modifier parameter (`ESC [1;5A` for Ctrl+Up), Home/End/PgUp/PgDn/Insert
  (they never reached apps before), right Ctrl/Alt, Shift+Tab as
  `ESC [Z`, and an ESC prefix for Alt+key. `ic_app` decodes them into
  `ev->mods` (`IC_MOD_SHIFT`, `IC_MOD_ALT`, `IC_MOD_CTRL`); Ctrl+letter
  arrives as the letter with `IC_MOD_CTRL`. The WM swallows a whole
  escape sequence when ESC closes an overlay. Editor: Shift-select with
  highlight, typing replaces the selection, Ctrl+A/S, Ctrl+Home/End,
  Ctrl+Left/Right by word. Terminal: Ctrl+C/L/U/A/E. Browser: Ctrl+L,
  Alt+Left/Right. No clipboard yet.
- Timezone: done. `tz=<minutes>` in the settings file (signed), set from
  Settings > Date & Time in half-hour steps. `ic_wallclock` applies it
  (re-read every 3 s, or at once after `ic_time_reload_tz()`), so the
  taskbar clock and every app follow it. The taskbar clock now refreshes
  when the hour or day changes, not only the minute.
- Menus: done. `ic_menu_model_t` has `checked[]` (`IC_MENU_CHECK_OFF/ON`,
  which also reserves the check column) and `submenu[]` (draws a
  chevron); `ic_ui_menu_item_rect()` positions a child. Menu models must
  be zero-initialised. The desktop menu groups "Add ..." under an
  "Add to Desktop" submenu; Explorer shows Icons/List with a check.
  Explorer's context menu used hard-coded indices that ran "New Folder"
  for most entries; it now maps entries to action ids.
- Launcher search: done. Typing while the launcher is open filters apps
  by substring; Enter launches the first match, Backspace edits.
- Snapping: done. Drag a title bar to the left or right edge for a half,
  or to the top to maximize; a translucent preview shows the target.
  Dragging a snapped window away restores its size.
- Overview: done. F11 (kernel sentinel `0x81`) shows every window as a
  scaled thumbnail; click one to bring it forward, Escape or F11 closes.

### Still open

Everything in P1-P3 is done or partly done as described above. What is
left, smallest first:
- Clipboard: done. `ic_clipboard_set/get` (ic_app) keep it in
  `/home/.clipboard`, so no new syscall was needed. The Editor has
  Ctrl+C/X/V (also Ctrl+V in its path prompt), and the Terminal pastes
  on right-click. The WM now forwards a whole escape sequence per frame,
  so End/Home/arrows no longer leak `[F`-style bytes into apps.
- True 2x rendering: done. `ic_canvas_t` has a `scale`; every `ic_gfx`
  primitive takes logical coordinates and draws at device resolution, so
  shapes, corners, shadows and blur are crisp. The WM composes a
  device-resolution scene, allocates window buffers at `w*scale x h*scale`,
  and sends `scale` in `GUI_MSG_OPEN_OK` and `GUI_MSG_RESIZE`. Text uses
  2x glyph atlases (`scripts/gen_fonts.py --blob2x` writes
  `resources/fonts/ui-2x.icf`, which the kernel seeds at
  `/usr/share/fonts/ui-2x.icf`). The WM loads that file once into shared
  memory and passes the handle to apps (`open_ok.font_shm`), so no app
  carries a copy. Layout still uses 1x metrics, so 1x and 2x line up.
  Icons are still upscaled from their 1x bitmaps. 2x is chosen by
  `scale=2`, or automatically at 2560 px wide and up.
- Explorer delete: done (see "Explorer delete" above). Folder rename is
  still missing.

### Partial writes (native ABI v6)
Saving no longer rewrites whole files. `vfs_sync()` only marks the tree
dirty, and `vfs_flush()` exports the persistfs bundle in three cases:
- once writes have been quiet for 1 s, and at most 5 s after the first
  change;
- on `SYS_SYNC`;
- before power-off.

On FAT32 and exFAT volumes, `vfs_node_write_at` and `vfs_node_truncate`
call `fatfs_write_at`/`fatfs_truncate` and
`exfat_write_at`/`exfat_truncate`. These grow cluster chains, zero-fill
gaps and rewrite only the clusters they touch. An exFAT NoFatChain file
stays contiguous when it can; otherwise it is converted to a FAT chain.

ABI v6 adds two calls:
- `SYS_VFS_WRITE_AT` = 77 (path, off, buf, len);
- `SYS_VFS_TRUNCATE` = 78 (path, len).

libc `FILE` tracks the dirty byte range, so `fopen` with `"a"` or `"r+"`
writes only the changed bytes. `"w"` still writes the whole file.
`libctest` covers this on /home, exFAT and FAT32.

The Linux personality also gets the `syscall` instruction
(`linux_syscall_entry`, which shares `syscall_common` with `int 0x80`) and
a per-thread FS base for TLS (`arch_prctl` SET_FS/GET_FS). The interrupt
paths no longer reload FS or GS, because on Intel that clears the base.

### Linux binaries (BusyBox)
`kernel/linux/lx.c` implements the Linux x86-64 syscall ABI for ELF files in
`/bin` that are not native (the OSABI byte is not 0xFF). It runs Debian's
static glibc BusyBox 1.35 unmodified (`resources/linux/busybox`, seeded as
`/bin/busybox`). `lx_init` adds a `#!/bin/busybox` stub for each common
applet (`/bin/sh`, `ls`, `vi`, …) plus `/etc/passwd` and `/etc/group`.

**Process start.** The loader passes a full auxv (AT_PHDR, AT_ENTRY,
AT_RANDOM, AT_PLATFORM, AT_EXECFN, …) and a default environment.
`user_enter` zeroes every register, because glibc treats `rdx` as an
atexit hook.

**Files.** Each process has its own fd table of reference-counted open
files: VFS files, the tty, pipes, `/dev/null`, `/dev/zero` and
`/dev/urandom`. `/proc` is synthetic (meminfo, uptime, loadavg, mounts,
cpuinfo, stat, plus `<pid>/stat`, `cmdline`, `status` and `comm`), so
`free`, `ps` and `uptime` work.

**tty.** A termios line discipline sits on top of the raw pty: canonical
mode with echo and erase, raw mode for `vi`, and TCGETS/TCSETS,
TIOCGWINSZ and TIOCGPGRP/TIOCSPGRP.

**Processes.**
- `fork`, `vfork` and `clone` (non-thread) use copy-on-write.
  `vmm_clone_user` shares frames and marks writable pages `VMM_COW`. The
  pmm keeps per-frame reference counts, and `vmm_cow_break` resolves
  write faults and kernel writes.
- `execve` (with `#!` scripts), `wait4`, `kill`, process groups and
  sessions are supported.
- Signals use real `rt_sigframe`/`rt_sigreturn` (with SA_RESTART and
  fxsave state) and are delivered when a syscall returns. Ctrl+C sends
  SIGINT to the newest Linux process on the pty.

**Memory.** User address spaces now have private low page tables, and the
identity map is no longer global, so static binaries can load at
0x400000. Exiting Linux processes free their address space. Pages above
`rsp` inside the stack region are mapped on demand.

**Not done yet:**
- threads (CLONE_VM without VFORK returns ENOSYS);
- `mmap` of shared files;
- symlinks;
- renaming on FAT/exFAT volumes;
- freeing process and thread structs after reaping (about 28 KB each);
- asynchronous signal delivery to a process spinning in user mode.

### Higher-half kernel and SMP

**Higher-half kernel.** The kernel is linked at `0xFFFFFFFF80000000 +
physical`; see `kernel/linker.ld`. Only the multiboot header and the 32-bit
bootstrap (`.boot*` in `boot.asm`) stay low. The bootstrap maps
PML4[511], jumps high, and moves onto the high alias of the boot stack.

Kernel C code is built with `-mcmodel=kernel -mno-red-zone`. The pmm
bitmap moves to the HHDM after `vmm_init` (`pmm_use_hhdm`). The whole
lower half now belongs to user programs. Before this change, a static
Linux binary at 0x400000 shadowed the kernel image (which runs to about
24 MB) whenever its address space was active.

**SMP.** `kernel/cpu/smp.c` starts every enabled MADT LAPIC with
INIT-SIPI-SIPI through `ap_trampoline.asm`. That code is a flat binary
copied to 0x8000: it goes from real mode to long mode on the kernel PML4
and calls `ap_entry`. Each CPU has a `cpu_t` holding:
- its own GDT and TSS;
- the current and pinned idle thread;
- its active address space;
- the syscall stack, which the `syscall` entry stub reaches through
  `swapgs` and KERNEL_GS_BASE.

`this_cpu()` maps the LAPIC ID to the CPU index.

**Big kernel lock.** Kernel code still assumes a single CPU, so a big
kernel lock (a fair ticket lock) serializes it:
- Every interrupt, exception and syscall entry takes the lock if this CPU
  does not already hold it.
- New threads and fork children drop it just before entering user mode.
- Idle threads drop it before `hlt`.

User code runs in parallel on all CPUs. The PIT tick is counted before the
lock is taken, so timeouts keep advancing.

**Timers.** APs get a calibrated periodic LAPIC timer on vector 48 for
preemption. Device IRQs stay on the BSP, which runs the 8259 through
LINT0 in virtual-wire mode.

**Testing note.** On a 4-thread host, running 4 busy vCPUs starves QEMU,
and PS/2 keys get dropped. That is an emulator limit; use `-smp 2` or
fewer busy jobs.

### Small fixes (names, rename, UTF-8, Ctrl+Shift)
- **Long names.** VFS and volume names can be 255 bytes (the limit was 63).
  Explorer paths can be 512 bytes, and a folder can list up to 1024 items.
- **Rename.**
  - `SYS_VFS_RENAME` = 79 (ABI v7) calls `vfs_rename`, which renames or
    moves any node and replaces an existing file target.
  - On volumes it calls `fatfs_rename` (new LFN entry on the same chain,
    old slots freed, `..` repointed when a folder moves) or `exfat_rename`
    (new entry set, old set marked unused).
  - Explorer renames folders and files with it, falling back to copy and
    delete for files only.
- **UTF-8.**
  - Font atlases cover Latin-1, common punctuation, €/™, arrows and box
    drawing: 259 glyphs, binary-searched in `ic_font.c`. Glyphs a font
    lacks render as `?`.
  - Kerning stays within the first 256 glyphs.
  - Terminal cells hold code points, and the output parser decodes UTF-8.
- **Ctrl+Shift.**
  - The keyboard driver sends Ctrl+Shift+letter as `ESC [27;6;<code>~`
    (xterm modifyOtherKeys), and `ic_app` turns that into a key event with
    CTRL|SHIFT.
  - In the Terminal, Ctrl+Shift+V pastes.
- **Linux `getdents64`/`stat`.** These synthesize an inode number for
  imported volume nodes, which have inode 0. glibc skips entries with
  inode 0, so before this fix volumes listed as empty.

Regenerating fonts needs network for pip. Inside Docker, pass
`--dns 8.8.8.8`:
`docker run --rm --dns 8.8.8.8 -v "$PWD:/workspace" -w /workspace python:3.12-slim sh -c 'pip install -q pillow fonttools && python scripts/gen_fonts.py && python scripts/gen_fonts.py --blob2x'`

### Surfer, milestone 1: networking

**Kernel sockets** (`kernel/net/sock.c`, `SYS_NET` = 80, ABI v8).
- One syscall carries the TCP and UDP operations: socket, connect, send,
  recv, close, poll, status, sendto, recvfrom and info.
- TCP supports many concurrent connections, with an ARP cache, MSS 1460,
  a 64 KB send buffer and receive window, retransmission with exponential
  RTO, slow start and congestion avoidance, and zero-window probes.
- Frames are pumped from the timer tick and from socket calls.
  `net_rx_frame()` hands frames the stack does not consume to the older
  blocking fetchers in `net.c`/`tls.c`.
- The e1000 rings went from 16 to 256 descriptors, because a 64 KB window
  used to overflow the ring and stall on RTO.
- `/dev/serial` writes go to the serial log, which serves as test output.

**Userspace library** (`userspace/surfer/`):
- `net.c` wraps the sockets and adds a UDP DNS resolver with a cache.
- `http.c` is an HTTP/1.1 client (redirects, chunked, URL resolution, one
  retry).
- `tls.c` derives from the kernel TLS, which still serves `curl`. It is TLS
  1.3 only for now (X25519, AES-128-GCM); the TLS 1.2 path still fails
  closed.
- `x509.c` plus `crypto/` (SHA-384/512, ECDSA P-256/P-384) verify the
  certificate chain to the Mozilla roots in `/etc/ssl/certs.pem`
  (`resources/ssl/cacert.pem`). This covers RSA PKCS#1 and PSS signatures,
  hostname (SAN and wildcard) checks, expiry against `/dev/rtc`, and the
  TLS 1.3 CertificateVerify.

**`/bin/fetch [-v] [-s] [-d] url...`** is the command-line client. `-s`
writes its result lines to the serial log, and `-d` prints TLS
diagnostics. `.verify/fetchrun.sh` boots ICDA, runs it and prints those
lines. `.verify/tlshost/` builds the same TLS/HTTP code against Linux
sockets for fast debugging.

**Measured in QEMU:** Wikipedia (635 KB) loads in 2.1 s, GitHub (577 KB)
in 1.5 s, and the handshake with verification takes about 0.3–0.5 s.
example.com times out from this Windows host even with Windows' own curl,
so its occasional failures come from the network path.

## Regenerating assets
```sh
python3 -m venv /tmp/v && /tmp/v/bin/pip install pillow fonttools
/tmp/v/bin/python scripts/gen_fonts.py            # -> userspace/ic_fonts_gen.h
python3 scripts/gen_icons.py --sheet /tmp/icons.png  # -> resources/icons, icon_data.h
```
