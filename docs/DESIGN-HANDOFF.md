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
| Activity | `taskman.c` | Monitor | no |
| Music | `audioplay.c` | List + now-playing | no |
| Editor | `editor.c` | Toolbar + text + status | no |
| Disk Utility | `diskman.c` | Toolbar + sidebar + detail | no |
| Browser | `browser.c` | Toolbar + address + page | no |
| Explorer | `desktop.c` | Toolbar + sidebar + grid/list | no |
| ICDA Demo | `gui_demo.c` | Control gallery (CI image) | no |

Harness notes: PS/2 motion is relative only, <=127 px per packet, and
one axis per `input-send-event` call, or QEMU drops the y event. The
first rel event after boot can be lost, so issue a warm-up move. A
double-click must keep both presses inside 400 ms (`DBLCLICK_TICKS` in
`wm.c` is 40 ticks at 100 Hz) or the WM reads two single clicks.

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
Blocked until the apps above are verified, since removing the API while
anything still calls it breaks the build. Remove from `libicda.[ch]`:
`ic_theme_t` / `ic_theme_default`, `IC_RADIUS_*` / `IC_TITLE_H` /
`IC_BTN_*` / `IC_ANIM_MAX` / `IC_WALL_*`, `ic_draw_chrome` + `ic_hit_*`,
`ic_draw_button`, `ic_menu_*`, `ic_dialog_draw`, `ic_slider_*`,
`ic_textfield_*`, `ic_listview_*`, `ic_scrollbar_draw`, `ic_rect_r` /
`ic_outline_r` / `ic_draw_shadow` / `ic_gradient_*` / `ic_blend*`,
`ic_text*` / `ic_font_*` (bitmap and old atlas), `font_atlas.h`, and
`ic_run_app`. Also drop `gui_draw_text` / `gui_draw_char` from `gui.[ch]`.
Then grep `userspace/*.c` for hex literals; the only exceptions allowed
are `ic_theme.c`, `wm_shell.c` (wallpaper glows) and the generators.

### P2: verification still owed
- The seven unverified apps above.
- `scripts/run-selftest-gates.sh` has not completed a clean run yet.
- WM interaction paths: maximize/restore round trip with buffer swap,
  edge resize, minimize/restore animation, close fade finaliser, context
  menu actions, Get Info alert, shutdown/restart overlay, and both
  1920x1080 and 1024x768 layouts.
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