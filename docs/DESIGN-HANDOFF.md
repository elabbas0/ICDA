# Design overhaul: status and remaining work

Branch: `dev`, uncommitted working tree. Scope: bring the shell and apps
to macOS-level visuals on a shared design system (see `docs/DESIGN.md`).
A clean build passes: `make clean && make kernel.iso`.

## 1. Done (built and booted in QEMU)

### Kernel
| Change | Files | Notes |
|---|---|---|
| x87/SSE enabled; FXSAVE/FXRSTOR per thread on context switch | `kernel/cpu/fpu.[ch]`, `proc/process.h` (`thread_t.fpu_state[512]`, appended at the end, so the asm offsets are unchanged), `proc/sched.c`, `kernel.c` | Userspace is now built with `-msse2 -mfpmath=sse` |
| **Bug fix:** the ISR/IRQ epilogue clobbered the interrupted RCX (`xor ecx,ecx` / `mov cx,..` after the pops) | `kernel/cpu/isr.asm` | Segments are now reloaded through RAX before the pops. This was likely the real cause of the "register corruption" workarounds commented throughout `wm.c` |
| **Bug fix:** timer EOI was deferred until after `schedule()`, which stalls the tick when the preempted thread is idle and another task yields | `kernel/cpu/isr.c` | IRQ0 is now EOI'd first |
| `/dev/rtc` readable node (CMOS clock), via a generic `dev_calls_t.node_read` hook in `SYS_VFS_READ` | `drivers/rtc/rtc.[ch]`, `dev/devops.h`, `dev/devnodes.c`, `syscall/syscall.c` | The native ABI is untouched (still 70 calls; `check-abi.sh` unaffected) |
| Makefile: the `diskman.app` recipe was attached to the `taskman.o` rule, which broke a clean build | `Makefile` | Fixed |

### Userspace design system (partially linked into `libicda.o`)
- `ic_time`: TSC clock calibrated against the tick; `ic_wallclock()` reads `/dev/rtc`.
- `ic_anim`: cubic-bezier easing, tweens, springs.
- `ic_gfx`: clip-aware antialiased rrects and strokes, circles, lines, gradients, masked/scaled/opacity blits, 3-pass box blur, cached 9-slice shadows, backdrop blur.
- `ic_font` + `ic_fonts_gen.h`: 13 named styles in Inter and JetBrains Mono, unhinted, with 2 subpixel phases and GPOS kerning. Generator: `scripts/gen_fonts.py`.
- `ic_theme`: semantic palette in dark and light, 7 accents, spacing/radius/size/motion/elevation tokens. The settings store gained `appearance=` and `accent=`.
- `ic_ui` + `ic_symbols`: buttons, toggle, segmented control, slider, progress, text field, groups, sidebar, list/table, toolbar, status bar, overlay scrollbar, menu, panel, alert, empty state; 33 vector symbols.
- `ic_app`: app runtime: typed events (press/release/leave, decoded arrow/Delete/Home/End keys, focus, resize, appearance), redraw pacing, caret blink.
- Icons: `scripts/gen_icons.py` rewritten for a 64 px family. The cursor/min/max/close/gear/shell/desktop/audio .ico files were removed.

### Window manager (`wm.c` restructured, plus `wm_frame.c` and `wm_shell.c`)
- Clip-aware damage compositing. Damage touching a blurred surface grows to cover the whole surface, so the blur never samples stale pixels.
- Frames: 34 px title bar, radius 10, hairline, two-layer shadow, centred title, caption buttons with hover/press. Actions fire on release.
- Time-based animations: open/close zoom and fade, minimize/restore to the taskbar entry, maximize with interpolated geometry.
- Real buffer resize (`GUI_MSG_RESIZE`) on maximize and on edge/corner drag resize. Before this, maximize broke the content stride.
- Double-clicking the title bar zooms; dragging a maximized window restores it.
- Pointer capture, release and leave events are forwarded to apps.
- Taskbar: material blur, stable open order, RTC clock and date, now playing. The launcher panel replaces the Start menu. Context menu and Get Info alert fade in.
- Live appearance and accent switching; the wallpaper has dark and light variants.
- Reference app migrated: `settings.c` (Appearance / Motion & Display / Sound / About).

## 2. Remaining work (priority order)

### P0: migrate the apps to `ic_app` + `ic_ui` (they still use the legacy UI)
Each is a self-contained rewrite of one file. Follow `docs/DESIGN.md` and use `settings.c` as the template. The window title must equal the label in `wm_apps[]` (`wm_shell.c`), otherwise the taskbar icon is wrong.

| File | Title | Shell (DESIGN.md) | Must keep |
|---|---|---|---|
| `terminal.c` | Terminal | Console, `IC_FONT_MONO` on `content` | Builtin commands, scrollback; add reflow on resize. It currently fills a hard-coded 500x300 |
| `taskman.c` | Activity | Monitor: toolbar + table + summary | Process list, kill/suspend/resume; confirm kills with `ic_ui_alert` |
| `audioplay.c` | Music | List + now-playing bar | Discovery, argv file, respect the `audio` setting, `icda_audio_info` progress |
| `editor.c` | Editor | Toolbar + text + status bar | argv path, save, navigation. Drop the manual ESC decoder (ic_app decodes keys) |
| `diskman.c` | Disk Utility | Toolbar + sidebar + detail | Every destructive operation behind an alert. Fix the existing `-Wreturn-type` warning at line 637 |
| `browser.c` | Browser | Toolbar + address field + page | Fetch, history, links, error states |
| `desktop.c` | Explorer | Toolbar + sidebar + grid/list | Navigation, open-by-type, context menu, rename/new/delete |
| `gui_demo.c` | ICDA Demo | Control gallery (CI image only) | Rewrite as a gallery of every `ic_ui` control, for visual review |

Process lesson: seven parallel agents were started on these and stopped mid-way; their edits were reverted. If you parallelise again, give each agent its own worktree **after committing the library**, and build only that agent's `<app>.o` / `userspace/<app>.app`. A shared tree plus concurrent `make` risks races on `libicda.o`.

### P1: remove the legacy UI API once all apps are migrated
From `libicda.[ch]`: `ic_theme_t` / `ic_theme_default`, `IC_RADIUS_*` / `IC_TITLE_H` / `IC_BTN_*` / `IC_ANIM_MAX` / `IC_WALL_*`, `ic_draw_chrome` + `ic_hit_*`, `ic_draw_button`, `ic_menu_*`, `ic_dialog_draw`, `ic_slider_*`, `ic_textfield_*`, `ic_listview_*`, `ic_scrollbar_draw`, `ic_rect_r` / `ic_outline_r` / `ic_draw_shadow` / `ic_gradient_*` / `ic_blend*`, `ic_text*` / `ic_font_*` (bitmap and old atlas), `font_atlas.h`, and `ic_run_app`.
Also drop `gui_draw_text` / `gui_draw_char` from `gui.[ch]`.
Then grep for hex literals in `userspace/*.c`; the only exceptions allowed are `ic_theme.c`, `wm_shell.c` (wallpaper glows) and the generators.

### P2: verification not yet done
- Run the CI gates locally: `sh scripts/qemu-smoke.sh kernel.iso`, plus the nptest/nptestlx self-tests from `.github/workflows/qemu.yml`. These cover the ISR epilogue, the EOI order and `sys_vfs_read`.
- Test on real hardware or QEMU+KVM on x86. On an arm64 Mac, QEMU is TCG-emulated and far too slow to judge frame pacing. Use the F12 overlay, which shows frame time.
- WM interaction paths still untested in the VM:
  - maximize/restore round trip and buffer swap with a legacy app;
  - edge resize;
  - minimize/restore animation;
  - close fade finaliser;
  - context menu actions;
  - Get Info alert;
  - shutdown/restart overlay;
  - 1920x1080 and 1024x768 layouts.
- Shm lifetime on resize: the WM only unmaps the old region and the app's `gui_apply_resize` frees it. An app that never processes `RESIZE` leaks a region (32 max). Consider reclaiming on window close.

### P3: polish and gaps
- Cursor: replace the polygon sprite with an antialiased sprite with a shadow, and add resize/text cursors (the WM knows the hit zone). The "black bar" beside the arrow is the sprite rim.
- Blur cost: `ic_gfx_backdrop` re-blurs the whole taskbar and panel on every damaged frame that touches them. Cache the blurred wallpaper strip under the taskbar and invalidate it only when the windows beneath change.
- Composite cost: zoom animations re-render the frame into `layer_buffer` for every damage region. Render once per frame.
- HiDPI: no scale factor yet. Add a logical-to-physical scale in `ic_canvas_t` / tokens, plus 2x font and icon atlases.
- Mouse wheel: the PS/2 IntelliMouse handshake is absent (`kernel/drivers/input/mouse.c`), so there is no scroll wheel or inertial scrolling. That needs a `dz` field in `icda_mouse_event_t` (append-only ABI struct change) or a new `/dev` node.
- Keyboard: no modifier info reaches apps (no Cmd/Ctrl shortcuts, no Shift-select). Consider an append-only extension of the `gui_msg_t.key` payload.
- Timezone: `/dev/rtc` is shown as-is (UTC on QEMU). Add a `timezone=` setting.
- `ic_ui_menu` has no submenus or checkmarks; the launcher has no search.
- Window snapping (drag to an edge to tile), and a window overview.

## 3. How to regenerate assets
```sh
python3 -m venv /tmp/v && /tmp/v/bin/pip install pillow fonttools
/tmp/v/bin/python scripts/gen_fonts.py            # -> userspace/ic_fonts_gen.h
python3 scripts/gen_icons.py --sheet /tmp/icons.png  # -> resources/icons, userspace/icon_data.h
```
