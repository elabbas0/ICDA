# Design overhaul: status

Branch: `dev`. The original tree arrived with no git history, so the
first commits bundle the import with work already done to it; see the
commit bodies for exactly what was imported versus rewritten.

Current state:

- **Builds clean.** `make` exits 0, `scripts/check-abi.sh` reports the
  70 native calls in sync, and every app compiles with zero warnings.
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
| Activity | `taskman.c` | Monitor | no |
| Editor | `editor.c` | Toolbar + text + status | no |
| Disk Utility | `diskman.c` | Toolbar + sidebar + detail | no |
| Browser | `browser.c` | Toolbar + address + page | no |
| ICDA Demo | `gui_demo.c` | Control gallery (CI image) | no |

What the three verified apps cover:

- Terminal: window opens, heading/hint/prompt render in the right roles,
  caret visible, scrollback reachable.
- Explorer: sidebar navigation to `/apps` lists all 14 `.app` files, the
  status bar reports `/apps  14 items`, both the icon grid and the list
  view render, and the toolbar buttons respond.
- Music: `.wav` discovery, track list, selection pill, disabled Stop
  while idle, now-playing bar, and correct reflow when the window is
  maximized (so `IC_EV_RESIZE` is handled).

Bugs this found and fixed, none of which the compiler or the smoke test
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

## Known gap: Explorer cannot delete

The VFS has no unlink primitive and the native ABI is frozen at exactly
70 syscalls (`scripts/check-abi.sh` fails at 71). Adding delete means
either a new syscall plus an ABI version bump, or an unlink inside the
VFS behind an existing call. Until that is decided the menu entry is
omitted and the Delete key says why. Rename is therefore
copy-then-truncate, which the status line also states.

## Remaining work

### P1: remove the legacy UI API

Done. The legacy `libicda` UI API has been removed; code comments were moved to
`docs/code-comments.md`. Then grep `userspace/*.c` for hex literals; the only exceptions allowed
are `ic_theme.c`, `wm_shell.c` (wallpaper glows) and the generators.

### P2: verification still owed
- The five unverified apps above (Activity, Editor, Disk Utility, Browser,
  ICDA Demo).
- `scripts/run-selftest-gates.sh` has not completed a clean run yet.
- WM interaction paths: caption buttons could not be confirmed, because
  aiming precisely enough at a 22px target through a lossy PS/2 mouse is
  impractical. Until they are exercised, treat minimize, maximize/restore,
  close and their fade animations as unverified. Note that a double-click
  on the title bar *did* zoom correctly, and the zoomed window reflowed
  its app correctly.
- Edge resize, context menu actions, Get Info alert, shutdown/restart
  overlay, and both 1920x1080 and 1024x768 layouts.
- Shm lifetime on resize: the WM unmaps the old region and the app's
  `gui_apply_resize` frees it, so an app that never processes RESIZE
  leaks a region (32 max).

### P3: polish
- Cursor: replace the polygon sprite with an antialiased sprite plus a
  shadow, and add resize/text cursors. The "black bar" beside the arrow
  is the sprite rim.
- Blur cost: `ic_gfx_backdrop` re-blurs the whole taskbar and panel on
  every damaged frame. Cache the blurred wallpaper strip.
- Composite cost: zoom animations re-render the frame per damage region;
  render once per frame.
- HiDPI: no scale factor. Add a logical-to-physical scale in
  `ic_canvas_t` and 2x font/icon atlases.
- Mouse wheel: the PS/2 IntelliMouse handshake is absent
  (`kernel/drivers/input/mouse.c`), so there is no wheel or inertial
  scrolling. Needs a `dz` field in `icda_mouse_event_t` (append-only).
- Keyboard: no modifier info reaches apps, so no Cmd/Ctrl shortcuts or
  Shift-select.
- Timezone: `/dev/rtc` is shown as-is. Add a `timezone=` setting.
- `ic_ui_menu` has no submenus or checkmarks; the launcher has no search.
- Window snapping and a window overview.

## Regenerating assets
```sh
python3 -m venv /tmp/v && /tmp/v/bin/pip install pillow fonttools
/tmp/v/bin/python scripts/gen_fonts.py            # -> userspace/ic_fonts_gen.h
python3 scripts/gen_icons.py --sheet /tmp/icons.png  # -> resources/icons, icon_data.h
```