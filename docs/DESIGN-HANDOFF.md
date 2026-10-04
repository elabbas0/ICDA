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

## Regenerating assets
```sh
python3 -m venv /tmp/v && /tmp/v/bin/pip install pillow fonttools
/tmp/v/bin/python scripts/gen_fonts.py            # -> userspace/ic_fonts_gen.h
python3 scripts/gen_icons.py --sheet /tmp/icons.png  # -> resources/icons, icon_data.h
```
