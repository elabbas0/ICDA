# ICDA design system

This document is the contract every ICDA surface follows: the shell
(window manager, taskbar, launcher, desktop) and every app. The goal is
one coherent product. The same type, spacing, colour roles, controls and
motion appear everywhere, so a new screen looks like it was always
there.

If something you need is missing, add a token or a control to the
library. Don't reach for a literal.

## Architecture

```
userspace/
  ic_time.[ch]     monotonic TSC clock + wall clock (/dev/rtc)
  ic_anim.[ch]     easing curves, tweens, springs (time based)
  ic_gfx.[ch]      rasteriser: clip-aware AA shapes, blits, blur, shadows
  ic_font.[ch]     type system over generated atlases (ic_fonts_gen.h)
  ic_theme.[ch]    design tokens: palette, spacing, radii, sizes, motion
  ic_ui.[ch]       controls, containers, menus, alerts
  ic_symbols.c     vector symbol set (ic_symbol_draw)
  ic_app.[ch]      app runtime: typed events, redraw pacing, caret blink
  libicda.[ch]     core library (memory, strings, io, ipc, http) and the
                   umbrella header that includes all of the above
  wm.c             compositor: state, input routing, damage, animation
  wm_frame.[ch]    window frames, caption buttons, hit-testing
  wm_shell.[ch]    wallpaper, desktop icons, taskbar, launcher, overlays
scripts/
  gen_fonts.py     Inter + JetBrains Mono -> userspace/ic_fonts_gen.h
  gen_icons.py     icon set -> resources/icons/*.ico + userspace/icon_data.h
```

`libicda.o` is a partial link of the core and every `ic_*` module, so
apps keep linking a single object. Apps include `libicda.h` only.

The kernel saves x87/SSE state per thread (`kernel/cpu/fpu.c`), and
userspace is built with SSE2. Float math is fine in UI code.

## Principles

1. **Semantic, not literal.** Ask the palette for a role
   (`p->label_secondary`, `p->separator`, `p->accent`). Never write a hex
   colour in app code.
2. **One type scale.** Use the named styles in `ic_font.h`. Never pick a
   pixel size.
3. **4 px grid.** Use `IC_SP_*` for spacing and the `IC_H_*` / `IC_R_*`
   tokens for sizes and radii.
4. **Layout in one place.** Every region comes from a `*_rect()` helper,
   and `draw()` and hit-testing both call the same helpers.
5. **Motion has meaning and a budget.** Use tweens with `IC_DUR_*` and
   `IC_EASE_*`. Animate state changes, not decoration.
6. **Quiet chrome, loud content.** Accent colour only for selection,
   primary actions, focus and progress.

## Colour roles (`ic_palette_t`)

| Role | Use for |
|------|---------|
| `window` | Window content background |
| `content` | Inset content: lists, text areas, terminal |
| `sidebar` | Source lists / navigation columns |
| `group` | Grouped settings cards (`ic_ui_group`) |
| `titlebar` | Title bar and in-window toolbars (unified look) |
| `material_menu`, `material_bar` | Tints over blur (menus, launcher, taskbar) |
| `control*` | Push-button faces and their states |
| `field` | Text-field face |
| `fill_hover`, `fill_pressed` | Translucent hover/pressed plates on any surface |
| `fill_selected_idle` | Selection when the list/sidebar is not focused |
| `label` | Primary text |
| `label_secondary` | Supporting text, subtitles, metadata |
| `label_tertiary` | Placeholders, disabled hints, empty-state copy |
| `label_disabled` | Disabled control text |
| `label_on_accent` | Text on accent fills |
| `separator` | Hairlines between rows and sections |
| `accent` (+ `_hover`, `_pressed`, `_soft`) | Primary buttons, selection, focus, progress |
| `success`, `warning`, `danger` | Status only (and destructive actions) |

Appearance (dark/light) and accent come from `/cfg/icda-settings`.
`ic_app_run` reloads them on focus and about once a second, then sends
`IC_EV_APPEARANCE`. Apps that cache colours must drop the cache on that
event. Most apps just redraw.

## Typography (`ic_font_style_t`)

| Style | Size / weight | Use for |
|-------|---------------|---------|
| `IC_FONT_LARGE_TITLE` | 28 bold | Hero numbers, splash |
| `IC_FONT_TITLE1` | 22 semibold | Page title at the top of a pane |
| `IC_FONT_TITLE2` | 17 semibold | Panel headers |
| `IC_FONT_TITLE3` | 15 semibold | Group headers, empty-state titles |
| `IC_FONT_HEADLINE` | 13 semibold | Window titles, emphasised row titles, section headers |
| `IC_FONT_BODY` | 13 regular | Default UI text, rows, buttons |
| `IC_FONT_BODY_EMPH` | 13 medium | Taskbar items, emphasis inside body |
| `IC_FONT_SUBHEAD` | 15 regular | Reading text (browser page body) |
| `IC_FONT_FOOTNOTE` | 12 regular | Subtitles, status bars, hints |
| `IC_FONT_CAPTION` / `_EMPH` | 11 | Table headers, metadata, badges |
| `IC_FONT_MONO` / `_SMALL` | 13 / 12 | Terminal, editor, code, byte counts |

- Place text with `ic_text_draw_in(c, face, rect, s, colour, align)`. It
  centres on the cap height and truncates with an ellipsis. Draw at a raw
  baseline only when composing multi-line blocks.
- Copy: Title Case for window titles, buttons and menu items; sentence
  case for descriptions. Keep internals out of the UI: no paths, driver
  names or tick counts unless the screen is about them. An ellipsis (…)
  means "asks for more input".

## Metrics

- Spacing: `IC_SP_1..IC_SP_8` = 4, 8, 12, 16, 20, 24, 32.
- Radii: window 10, panel 12, menu 8, menu item 5, group 9, control 6, row 6.
- Heights: control 26 (compact 22), row 28, tall row 44, menu item 24,
  toolbar 44, title bar 34, taskbar 48. Sidebar width 180.
- Icons: 16 (inline), 20, 32, 48 (desktop), 64 (native). App icons come
  from `ic_icon_builtin(name)`. Symbols come from
  `ic_symbol_draw(c, IC_SYM_*, cx, cy, size, colour)`.

## Window anatomy

```
+----------------------------------------------+
|              Title             -  []  x      |  WM title bar (34)
+----------------------------------------------+
| toolbar (optional, IC_H_TOOLBAR, titlebar    |  ic_ui_toolbar
| colour so it merges with the title bar)      |
+-----------+----------------------------------+
| sidebar   | content (window colour)          |
| 180       |  Title1 at (x+24, y+20)          |
|           |  groups/lists with 24 px margins |
+-----------+----------------------------------+
| status bar (optional, 24, footnote text)     |  ic_ui_statusbar
+----------------------------------------------+
```

Pick the shell that fits the app:
- **Utility / preferences**: sidebar + grouped rows (Settings).
- **Document**: toolbar + content + status bar (Editor, Browser).
- **Browser of things**: toolbar + sidebar + list/grid (Explorer, Disk Utility).
- **Monitor**: toolbar + table + summary footer (Activity).
- **Console**: content only, `content` colour, mono font (Terminal).

## Controls (`ic_ui.h`)

- Buttons: `ic_ui_button` with `IC_BUTTON_PRIMARY` for the one default
  action per view, `DEFAULT` for others, `DESTRUCTIVE` for irreversible
  actions, and `PLAIN` in toolbars. Size with `ic_ui_button_width`.
- Toolbars: `ic_ui_icon_button` (28x28) plus plain buttons, and a
  `IC_SYM_SEARCH` text field for filtering.
- Switches: `ic_ui_toggle`, animated with a tween over `IC_DUR_BASE`.
  Use a switch for settings that apply immediately.
- Choice among 2 to 4 options: `ic_ui_segmented`, with the slide animated.
- Lists: `ic_ui_list_row` returns the text colour to use. Rows inset
  5 px, with the accent pill only while the list has focus.
- Tables: `ic_ui_table_header` plus list rows. Numbers are
  right-aligned in `IC_FONT_BODY` or mono.
- Progress: `ic_ui_progress`. Levels: `ic_ui_slider`.
- Scrolling: `ic_ui_scrollbar` (an overlay pill; fade it out when idle).
- Menus: `ic_ui_menu` over an `ic_menu_model_t`. Alerts: `ic_ui_alert`.
- Empty / error states: `ic_ui_empty_state` (symbol, title, one line).

State comes from pointer facts: `ic_ui_state(enabled, hover, pressed)`.
Show hover on everything clickable. Show pressed while the button is held.

## Motion

| Token | ms | Use |
|-------|----|-----|
| `IC_DUR_INSTANT` | 80 | Hover and press feedback, menu close |
| `IC_DUR_FAST` | 140 | Menus and popovers opening, small fades |
| `IC_DUR_BASE` | 220 | Toggles, segmented slide, window open |
| `IC_DUR_SLOW` | 340 | Minimize/restore, large moves |

Curves: `IC_EASE_ENTER` (decelerate) for things appearing,
`IC_EASE_EXIT` (accelerate) for things leaving, `IC_EASE_MOVE`
(emphasized) for things moving.

In app code:

```c
ic_tween_to(&row_hover, 1.0f, IC_DUR_INSTANT, IC_EASE_STANDARD);
...
void draw(ic_app_t *app, ic_canvas_t *c) {
    float h = ic_tween_value(&row_hover);
    ...
    if (ic_tween_running(&row_hover)) ic_app_animate(app);
}
```

The WM honours the "Window animations" setting. Apps keep their control
transitions short enough that they never block input.

## App skeleton

```c
#include "libicda.h"

static void draw(ic_app_t *app, ic_canvas_t *c) { /* paint everything */ }
static void event(ic_app_t *app, const ic_event_t *ev) { /* update, then */ ic_app_invalidate(app); }

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Title", 640, 440, init, draw, event, tick };
    (void)argc; (void)argv;
    return ic_app_run(&desc, 0) == 0 ? 0 : 1;
}
```

- The window title must match the app's label in `wm_shell.c` (`wm_apps`),
  so the taskbar shows the right icon.
- Events: `IC_EV_MOUSE_MOVE/DOWN/UP/LEAVE` (pointer capture while held,
  so drags work outside the window), `IC_EV_KEY` with decoded
  `IC_KEY_*` navigation keys, `IC_EV_FOCUS/BLUR`, `IC_EV_RESIZE` (the
  window is resizable, so lay out from `app->width/height`), and
  `IC_EV_APPEARANCE`.
- `settings.c` is the reference implementation.

## Shell

- Wallpaper is procedural and has a dark and a light variant (`wm_shell.c`).
- Taskbar: launcher at the left, then running windows in open order
  (stable, never z-order), then now-playing and the clock at the right.
- Launcher: an 8-app grid plus power actions, fading in on a material
  panel.
- Window animations: open and close zoom (0.94 to 1) with a fade;
  minimize and restore fly to and from the taskbar entry; maximize
  interpolates the frame and scales the content; double-clicking the
  title bar zooms.

## Assets

- **Fonts**: `python3 scripts/gen_fonts.py`. It needs Pillow and
  fontTools, and its output is committed. Glyphs are rendered unhinted at
  8x, with 2 subpixel phases and GPOS kerning.
- **Icons**: `python3 scripts/gen_icons.py [--sheet out.png]`. It needs
  Pillow. All icons share one 64 px template: a tile, a top-lit gradient,
  a contact shadow and a white glyph.
- Inter and JetBrains Mono ship under the SIL OFL 1.1 (see
  `resources/fonts/*-OFL.txt`).
