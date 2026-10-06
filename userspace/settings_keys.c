/*
 * Settings > Keyboard: the global shortcuts (shortcuts.h).  Click a row and
 * press the new combination; the window manager hands chords to Settings
 * while SC_CAPTURE_FLAG exists.  Esc cancels, Backspace turns the shortcut
 * off, and Reset brings back the defaults.
 */

#include "settings_keys.h"
#include "shortcuts.h"

#define ROW_H    34
#define FOOT_H   58

static struct {
    sc_bindings_t b;
    int capture;          /* row being recorded, -1 none */
    int hover;            /* row, -2 reset */
    int scroll;
    char notice[96];
} k;

static void capture_end(void) {
    if (k.capture >= 0) (void)icda_remove(SC_CAPTURE_FLAG);
    k.capture = -1;
}

void keys_pane_enter(void) {
    sc_load(&k.b);
    k.capture = -1;
    k.hover = -1;
    k.scroll = 0;
    k.notice[0] = 0;
}

void keys_pane_leave(void) {
    capture_end();
}

static ic_rect_t list_rect(ic_rect_t a) {
    return ic_rect_make(a.x, a.y, a.w, a.h - FOOT_H);
}

static int visible_rows(ic_rect_t a) {
    return list_rect(a).h / ROW_H;
}

static ic_rect_t row_rect(ic_rect_t a, int i) {
    ic_rect_t l = list_rect(a);
    return ic_rect_make(l.x, l.y + i * ROW_H, l.w, ROW_H);
}

static ic_rect_t reset_rect(ic_rect_t a) {
    return ic_rect_make(a.x + a.w - 150, a.y + a.h - FOOT_H + 18, 150, IC_H_CONTROL);
}

void keys_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t a) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t l = list_rect(a);
    int rows = visible_rows(a), count = SC_COUNT - k.scroll;
    (void)app;
    if (count > rows) count = rows;
    l.h = count * ROW_H;
    ic_ui_group(c, l);
    for (int i = 0; i < count; i++) {
        int idx = k.scroll + i;
        ic_rect_t r = row_rect(a, i);
        char combo[40];
        int w;
        ic_ui_group_row(c, r, i, count, k.hover == i ? 1.0f : 0.0f);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r.x + IC_SP_3, r.y, r.w / 2, r.h),
                        sc_actions[idx].label, p->label, IC_ALIGN_LEFT);
        if (k.capture == idx) {
            ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(r.x + r.w / 2, r.y, r.w / 2 - IC_SP_3, r.h),
                            "Press a shortcut...", p->accent, IC_ALIGN_RIGHT);
            continue;
        }
        if (k.b.set[idx]) sc_format(k.b.mods[idx], k.b.key[idx], combo, sizeof combo);
        else ic_strcpy(combo, "Off", sizeof combo);
        w = ic_text_measure(ic_font(IC_FONT_FOOTNOTE), combo) + 18;
        ic_gfx_rrect(c, r.x + r.w - IC_SP_3 - w, r.y + 7, w, r.h - 14, 6.0f, p->fill_hover);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + r.w - IC_SP_3 - w, r.y, w, r.h), combo,
                        k.b.set[idx] ? p->label : p->label_tertiary, IC_ALIGN_CENTER);
    }
    if (SC_COUNT > rows)
        ic_ui_scrollbar(c, ic_rect_make(l.x, l.y, l.w, rows * ROW_H), k.scroll * ROW_H, SC_COUNT * ROW_H, 1.0f);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(a.x, a.y + a.h - FOOT_H + 6, a.w - 170, 16),
                    k.notice[0] ? k.notice : "Use the Windows key, Ctrl+Alt, Alt+F-key or F1-F10.",
                    k.notice[0] ? p->accent : p->label_secondary, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(a.x, a.y + a.h - FOOT_H + 26, a.w - 170, 16),
                    "Click a shortcut to change it. Esc cancels, Backspace turns it off.",
                    p->label_secondary, IC_ALIGN_LEFT);
    ic_ui_button(c, reset_rect(a), "Reset to defaults", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 k.hover == -2 ? IC_STATE_HOVER : IC_STATE_NORMAL);
}

static void assign(int idx, uint8_t mods, uint8_t key) {
    for (int a = 0; a < SC_COUNT; a++) {
        if (a != idx && k.b.set[a] && k.b.mods[a] == mods && k.b.key[a] == key) {
            k.b.set[a] = 0;      /* one action per combination */
            ic_strcpy(k.notice, "That shortcut moved from \"", sizeof k.notice);
            ic_strlcat(k.notice, sc_actions[a].label, sizeof k.notice);
            ic_strlcat(k.notice, "\"", sizeof k.notice);
        }
    }
    k.b.mods[idx] = mods;
    k.b.key[idx] = key;
    k.b.set[idx] = 1;
    (void)sc_save(&k.b);
    ic_sound("toggle_on");
}

static int hit(ic_rect_t a, int x, int y) {
    int rows = visible_rows(a);
    if (ic_ui_hit(reset_rect(a), x, y)) return -2;
    for (int i = 0; i < rows && k.scroll + i < SC_COUNT; i++)
        if (ic_ui_hit(row_rect(a, i), x, y)) return i;
    return -1;
}

int keys_pane_modal(void) {
    return k.capture >= 0;
}

int keys_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t a) {
    (void)app;
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
    case IC_EV_MOUSE_LEAVE: {
        int h = ev->type == IC_EV_MOUSE_LEAVE ? -1 : hit(a, ev->x, ev->y);
        if (h == k.hover) return 0;
        k.hover = h;
        return 1;
    }
    case IC_EV_MOUSE_DOWN: {
        int h = hit(a, ev->x, ev->y);
        if (h == -2) {
            capture_end();
            (void)icda_remove(SC_PATH);
            sc_load(&k.b);
            ic_strcpy(k.notice, "Shortcuts reset to their defaults", sizeof k.notice);
            ic_sound("toggle_off");
            return 1;
        }
        if (h >= 0) {
            k.capture = k.scroll + h;
            k.notice[0] = 0;
            (void)icda_mkdir("/cfg");
            (void)icda_write_file(SC_CAPTURE_FLAG, "1", 1);
            return 1;
        }
        capture_end();
        return 0;
    }
    case IC_EV_KEY:
        if (k.capture < 0) return 0;
        if (ev->key >= SC_APP_KEY_BASE) {
            uint8_t mods = (uint8_t)((ev->key >> 8) & 0x0F), key = (uint8_t)(ev->key & 0xFF);
            int idx = k.capture;
            capture_end();
            assign(idx, mods, key);
        } else if (ev->key == IC_KEY_ESCAPE) {
            capture_end();
        } else if (ev->key == IC_KEY_BACKSPACE) {
            k.b.set[k.capture] = 0;
            (void)sc_save(&k.b);
            capture_end();
            ic_sound("toggle_off");
        } else {
            ic_strcpy(k.notice, "Add the Windows key or Ctrl+Alt to that key", sizeof k.notice);
        }
        return 1;
    case IC_EV_SCROLL: {
        int rows = visible_rows(a);
        if (SC_COUNT <= rows) return 0;
        k.scroll += ev->wheel > 0 ? 1 : -1;
        if (k.scroll > SC_COUNT - rows) k.scroll = SC_COUNT - rows;
        if (k.scroll < 0) k.scroll = 0;
        return 1;
    }
    case IC_EV_BLUR:
        capture_end();
        return 1;
    default:
        return 0;
    }
}
