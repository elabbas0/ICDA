












#include "libicda.h"
#include "settings_store.h"

#define WIN_W 700
#define WIN_H 480


#define PANE_PAD      IC_SP_6
#define PANE_TITLE_Y  IC_SP_5
#define GROUP_Y       72
#define GROUP_GAP     IC_SP_6
#define ROW_H         IC_H_ROW_TALL

enum { PANE_APPEARANCE = 0, PANE_MOTION, PANE_SOUND, PANE_TIME, PANE_ABOUT, PANE_COUNT };

static const char *const pane_titles[PANE_COUNT] = {
    "Appearance", "Motion & Display", "Sound", "Date & Time", "About"
};
static const ic_symbol_t pane_symbols[PANE_COUNT] = {
    IC_SYM_SUN, IC_SYM_ACTIVITY, IC_SYM_SPEAKER, IC_SYM_GLOBE, IC_SYM_INFO
};


typedef struct {
    int         pane;
    const char *title;
    const char *subtitle;
    ic_symbol_t symbol;
    uint32_t    tint;
} toggle_row_t;

enum { TOG_ANIMATIONS = 0, TOG_BOOT_ANIM, TOG_VSYNC, TOG_AUDIO, TOG_COUNT };

static const toggle_row_t toggles[TOG_COUNT] = {
    { PANE_MOTION, "Window animations", "Open, close, minimize and zoom with motion",
      IC_SYM_MAXIMIZE, IC_TINT_INDIGO },
    { PANE_MOTION, "Startup and shutdown", "Fade the screen when powering off or restarting",
      IC_SYM_POWER, IC_TINT_ORANGE },
    { PANE_MOTION, "Sync to display refresh", "Present once per frame to avoid tearing",
      IC_SYM_RELOAD, IC_TINT_TEAL },
    { PANE_SOUND, "Sound", "Allow apps to play audio",
      IC_SYM_SPEAKER, IC_TINT_PINK },
};

typedef struct {
    icda_settings_t s;
    int             pane;
    int             hover_pane;
    int             hover_row;        
    int             hover_swatch;
    int             hover_segment;
    int             hover_tz;
    ic_tween_t      toggle_pos[TOG_COUNT];
    ic_tween_t      segment_pos;
    ic_tween_t      row_hover[4];
    int             save_failed;
} settings_t;

static settings_t st;

static int *toggle_value(int t) {
    switch (t) {
    case TOG_ANIMATIONS: return &st.s.animations;
    case TOG_BOOT_ANIM:  return &st.s.boot_anim;
    case TOG_VSYNC:      return &st.s.vsync;
    default:             return &st.s.audio;
    }
}

static void save(void) {
    st.save_failed = icda_settings_save(&st.s) != 0;
}



static ic_rect_t sidebar_rect(ic_app_t *app) {
    return ic_rect_make(0, 0, IC_W_SIDEBAR, app->height);
}

static ic_rect_t sidebar_item_rect(int i) {
    return ic_rect_make(0, IC_SP_3 + i * (IC_H_ROW + 2), IC_W_SIDEBAR, IC_H_ROW);
}

static int content_x(void) {
    return IC_W_SIDEBAR + PANE_PAD;
}

static int content_w(ic_app_t *app) {
    int w = app->width - content_x() - PANE_PAD;
    return w > 560 ? 560 : w;
}

static ic_rect_t group_rect(ic_app_t *app, int y, int rows) {
    return ic_rect_make(content_x(), y, content_w(app), rows * ROW_H);
}

static ic_rect_t row_rect(ic_rect_t group, int i) {
    return ic_rect_make(group.x, group.y + i * ROW_H, group.w, ROW_H);
}


static ic_rect_t appearance_group(ic_app_t *app) {
    return group_rect(app, GROUP_Y, 2);
}

static ic_rect_t segmented_rect(ic_app_t *app) {
    ic_rect_t r = row_rect(appearance_group(app), 0);
    int w = 176;
    return ic_rect_make(r.x + r.w - IC_SP_3 - w, r.y + (r.h - IC_H_CONTROL_SM) / 2, w,
                        IC_H_CONTROL_SM);
}

#define SWATCH_D   18
#define SWATCH_GAP 10

static ic_rect_t swatch_rect(ic_app_t *app, int i) {
    ic_rect_t r = row_rect(appearance_group(app), 1);
    int total = IC_ACCENT_COUNT * SWATCH_D + (IC_ACCENT_COUNT - 1) * SWATCH_GAP;
    int x0 = r.x + r.w - IC_SP_3 - total;
    return ic_rect_make(x0 + i * (SWATCH_D + SWATCH_GAP), r.y + (r.h - SWATCH_D) / 2, SWATCH_D,
                        SWATCH_D);
}


static int pane_toggles(int pane, int *out) {
    int n = 0;
    for (int t = 0; t < TOG_COUNT; t++) {
        if (toggles[t].pane == pane) out[n++] = t;
    }
    return n;
}

static ic_rect_t toggle_switch_rect(ic_rect_t row) {
    return ic_ui_toggle_rect(row.x + row.w - IC_SP_3 - IC_TOGGLE_W, row.y + (row.h - IC_TOGGLE_H) / 2);
}



static void draw_sidebar(ic_app_t *app, ic_canvas_t *c) {
    ic_ui_sidebar_bg(c, sidebar_rect(app));
    for (int i = 0; i < PANE_COUNT; i++) {
        ic_ui_sidebar_item(c, sidebar_item_rect(i), pane_symbols[i], pane_titles[i], i == st.pane,
                           i == st.hover_pane ? 1.0f : 0.0f);
    }
}

static void draw_pane_title(ic_canvas_t *c, const char *title) {
    const ic_face_t *f = ic_font(IC_FONT_TITLE1);
    ic_text_draw(c, f, content_x(), PANE_TITLE_Y + f->ascent, title, ic_palette()->label);
}

static void draw_row_hover(ic_canvas_t *c, ic_rect_t row, int i, int count) {
    float h = i < 4 ? ic_tween_value(&st.row_hover[i]) : 0.0f;
    ic_ui_group_row(c, row, i, count, h);
}

static void draw_appearance(ic_app_t *app, ic_canvas_t *c) {
    static const char *const modes[2] = { "Dark", "Light" };
    ic_rect_t g = appearance_group(app);
    ic_rect_t r0 = row_rect(g, 0), r1 = row_rect(g, 1);
    const ic_palette_t *p = ic_palette();

    ic_ui_group(c, g);
    draw_row_hover(c, r0, 0, 2);
    draw_row_hover(c, r1, 1, 2);
    ic_ui_row_text(c, r0, st.s.appearance ? IC_SYM_SUN : IC_SYM_MOON, IC_RGB(IC_TINT_INDIGO),
                   "Appearance", "Use a dark or light look everywhere");
    ic_ui_segmented(c, segmented_rect(app), modes, 2, ic_tween_value(&st.segment_pos),
                    st.hover_segment);

    ic_ui_row_text(c, r1, IC_SYM_GRID, p->accent, "Accent color",
                   "Selection, buttons and focus");
    for (int i = 0; i < IC_ACCENT_COUNT; i++) {
        ic_rect_t s = swatch_rect(app, i);
        float cx = (float)s.x + SWATCH_D * 0.5f, cy = (float)s.y + SWATCH_D * 0.5f;
        ic_color_t col = ic_accent_swatch((ic_accent_t)i);
        if (i == st.s.accent) {
            ic_gfx_ring(c, cx, cy, SWATCH_D * 0.5f + 3.5f, 2.0f, col);
        } else if (i == st.hover_swatch) {
            ic_gfx_ring(c, cx, cy, SWATCH_D * 0.5f + 3.5f, 2.0f, p->separator);
        }
        ic_gfx_circle(c, cx, cy, SWATCH_D * 0.5f, col);
        if (i == st.s.accent) ic_symbol_draw(c, IC_SYM_CHECK, cx, cy, 11.0f, IC_WHITE);
    }

    
    {
        ic_rect_t pv = ic_rect_make(g.x, g.y + g.h + GROUP_GAP, g.w, 76);
        ic_rect_t b1, b2;
        ic_ui_section_header(c, pv.x, pv.y, "Preview");
        pv.y += 22;
        pv.h -= 22;
        ic_ui_group(c, pv);
        b1 = ic_rect_make(pv.x + IC_SP_4, pv.y + (pv.h - IC_H_CONTROL) / 2,
                          ic_ui_button_width("Cancel", IC_SYM_NONE), IC_H_CONTROL);
        b2 = ic_rect_make(b1.x + b1.w + IC_SP_2, b1.y, ic_ui_button_width("Continue", IC_SYM_NONE),
                          IC_H_CONTROL);
        ic_ui_button(c, b1, "Cancel", IC_SYM_NONE, IC_BUTTON_DEFAULT, IC_STATE_NORMAL);
        ic_ui_button(c, b2, "Continue", IC_SYM_NONE, IC_BUTTON_PRIMARY, IC_STATE_NORMAL);
        ic_ui_toggle(c, pv.x + pv.w - IC_SP_4 - IC_TOGGLE_W, pv.y + (pv.h - IC_TOGGLE_H) / 2, 1.0f,
                     IC_STATE_NORMAL);
        ic_ui_slider(c, ic_rect_make(b2.x + b2.w + IC_SP_6, pv.y, pv.x + pv.w - IC_SP_4 - IC_TOGGLE_W -
                                     IC_SP_6 - (b2.x + b2.w + IC_SP_6), pv.h), 0.62f, IC_STATE_NORMAL);
    }
}

static void draw_toggles(ic_app_t *app, ic_canvas_t *c, int pane) {
    int rows[TOG_COUNT];
    int n = pane_toggles(pane, rows);
    ic_rect_t g = group_rect(app, GROUP_Y, n);
    ic_ui_group(c, g);
    for (int i = 0; i < n; i++) {
        const toggle_row_t *t = &toggles[rows[i]];
        ic_rect_t r = row_rect(g, i);
        ic_rect_t sw = toggle_switch_rect(r);
        draw_row_hover(c, r, i, n);
        ic_ui_row_text(c, ic_rect_make(r.x, r.y, sw.x - r.x - IC_SP_3, r.h), t->symbol,
                       IC_RGB(t->tint), t->title, t->subtitle);
        ic_ui_toggle(c, sw.x, sw.y, ic_tween_value(&st.toggle_pos[rows[i]]),
                     st.hover_row == i ? IC_STATE_HOVER : IC_STATE_NORMAL);
    }
}

static ic_rect_t tz_group(ic_app_t *app) {
    return group_rect(app, GROUP_Y, 2);
}

static ic_rect_t tz_button_rect(ic_app_t *app, int plus) {
    ic_rect_t r = row_rect(tz_group(app), 0);
    int x = r.x + r.w - IC_SP_3 - IC_H_CONTROL_SM;
    if (!plus) x -= IC_H_CONTROL_SM + 96;
    return ic_rect_make(x, r.y + (r.h - IC_H_CONTROL_SM) / 2, IC_H_CONTROL_SM, IC_H_CONTROL_SM);
}

static void tz_label(int minutes, char *out, int cap) {
    int v = minutes < 0 ? -minutes : minutes;
    char n[8];
    out[0] = 0;
    ic_strlcat(out, minutes < 0 ? "UTC-" : "UTC+", cap);
    n[0] = (char)('0' + (v / 60) / 10);
    n[1] = (char)('0' + (v / 60) % 10);
    n[2] = ':';
    n[3] = (char)('0' + (v % 60) / 10);
    n[4] = (char)('0' + (v % 60) % 10);
    n[5] = 0;
    ic_strlcat(out, n, cap);
}

static void draw_time(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t g = tz_group(app);
    ic_rect_t r0 = row_rect(g, 0), r1 = row_rect(g, 1);
    ic_rect_t minus = tz_button_rect(app, 0), plus = tz_button_rect(app, 1);
    ic_datetime_t now;
    char label[16];
    char hm[8];
    char day[24];

    ic_ui_group(c, g);
    ic_ui_row_text(c, ic_rect_make(r0.x, r0.y, minus.x - r0.x - IC_SP_3, r0.h), IC_SYM_GLOBE,
                   IC_RGB(IC_TINT_TEAL), "Time zone", "Offset from UTC, in half hours");
    ic_ui_icon_button(c, minus, IC_SYM_MINUS, st.hover_tz == 0 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_icon_button(c, plus, IC_SYM_PLUS, st.hover_tz == 1 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    tz_label(st.s.tz_minutes, label, sizeof(label));
    ic_text_draw_in(c, ic_font(IC_FONT_MONO_SMALL),
                    ic_rect_make(minus.x + minus.w, r0.y, plus.x - minus.x - minus.w, r0.h), label,
                    p->label, IC_ALIGN_CENTER);
    ic_gfx_hline(c, r1.x + IC_SP_3, r1.y, r1.w - IC_SP_3, p->separator);
    hm[0] = day[0] = 0;
    if (ic_wallclock(&now) == 0) {
        ic_format_hm(&now, hm, sizeof(hm));
        ic_format_day(&now, day, sizeof(day));
    }
    ic_ui_row_text(c, ic_rect_make(r1.x, r1.y, r1.w / 2, r1.h), IC_SYM_SUN, IC_RGB(IC_TINT_ORANGE),
                   "Local time", day[0] ? day : "The clock is not available");
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r1.x + r1.w / 2, r1.y, r1.w / 2 - IC_SP_3, r1.h),
                    hm, p->label_secondary, IC_ALIGN_RIGHT);
}

static void set_tz(int minutes) {
    if (minutes < -12 * 60) minutes = -12 * 60;
    if (minutes > 14 * 60) minutes = 14 * 60;
    if (minutes == st.s.tz_minutes) return;
    st.s.tz_minutes = minutes;
    save();
    ic_time_reload_tz();
}

static void draw_about(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    const ic_icon_t *icon = ic_icon_builtin("settings");
    int cx = content_x() + content_w(app) / 2;
    int y = GROUP_Y + IC_SP_2;
    if (icon && ic_icon_valid(icon)) {
        ic_gfx_image_rgba(c, cx - 36, y, 72, 72, icon->rgba, icon->w, icon->h, 255);
    }
    y += 72 + IC_SP_4;
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE1), ic_rect_make(content_x(), y, content_w(app), 28),
                    "ICDA", p->label, IC_ALIGN_CENTER);
    y += 30;
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(content_x(), y, content_w(app), 18),
                    "Version " IC_VERSION_STRING, p->label_secondary, IC_ALIGN_CENTER);
    y += 36;
    {
        ic_rect_t g = group_rect(app, y, 3);
        static const char *const keys[3] = { "Kernel", "Interface", "Typefaces" };
        static const char *const vals[3] = { "ICDA x86_64", "ICDA design system",
                                             "Inter, JetBrains Mono" };
        ic_ui_group(c, g);
        for (int i = 0; i < 3; i++) {
            ic_rect_t r = row_rect(g, i);
            r.h = ROW_H;
            ic_ui_group_row(c, r, i, 3, 0.0f);
            ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r.x + IC_SP_3, r.y, r.w / 2, r.h),
                            keys[i], p->label, IC_ALIGN_LEFT);
            ic_text_draw_in(c, ic_font(IC_FONT_BODY),
                            ic_rect_make(r.x + r.w / 2, r.y, r.w / 2 - IC_SP_3, r.h), vals[i],
                            p->label_secondary, IC_ALIGN_RIGHT);
        }
    }
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_sidebar(app, c);
    draw_pane_title(c, pane_titles[st.pane]);
    if (st.pane == PANE_APPEARANCE) draw_appearance(app, c);
    else if (st.pane == PANE_ABOUT) draw_about(app, c);
    else if (st.pane == PANE_TIME) draw_time(app, c);
    else draw_toggles(app, c, st.pane);

    if (st.save_failed) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(content_x(), app->height - IC_SP_8, content_w(app), 18),
                        "Settings could not be saved on this device.", p->danger, IC_ALIGN_LEFT);
    }

    
    for (int t = 0; t < TOG_COUNT; t++) {
        if (ic_tween_running(&st.toggle_pos[t])) ic_app_animate(app);
    }
    for (int i = 0; i < 4; i++) {
        if (ic_tween_running(&st.row_hover[i])) ic_app_animate(app);
    }
    if (ic_tween_running(&st.segment_pos)) ic_app_animate(app);
}



static int current_rows(ic_app_t *app, ic_rect_t *rows) {
    if (st.pane == PANE_APPEARANCE) {
        ic_rect_t g = appearance_group(app);
        rows[0] = row_rect(g, 0);
        rows[1] = row_rect(g, 1);
        return 2;
    }
    if (st.pane == PANE_ABOUT || st.pane == PANE_TIME) return 0;
    {
        int ids[TOG_COUNT];
        int n = pane_toggles(st.pane, ids);
        ic_rect_t g = group_rect(app, GROUP_Y, n);
        for (int i = 0; i < n; i++) rows[i] = row_rect(g, i);
        return n;
    }
}

static void set_row_hover(int row) {
    if (row == st.hover_row) return;
    for (int i = 0; i < 4; i++) {
        ic_tween_to(&st.row_hover[i], i == row ? 1.0f : 0.0f, IC_DUR_INSTANT, IC_EASE_STANDARD);
    }
    st.hover_row = row;
}

static void update_hover(ic_app_t *app, int x, int y) {
    ic_rect_t rows[4];
    int n = current_rows(app, rows);
    int row = -1;
    st.hover_pane = -1;
    for (int i = 0; i < PANE_COUNT; i++) {
        if (ic_ui_hit(sidebar_item_rect(i), x, y)) st.hover_pane = i;
    }
    for (int i = 0; i < n; i++) {
        if (ic_ui_hit(rows[i], x, y)) row = i;
    }
    

    set_row_hover(st.pane == PANE_APPEARANCE ? -1 : row);
    st.hover_swatch = -1;
    st.hover_segment = -1;
    st.hover_tz = -1;
    if (st.pane == PANE_TIME) {
        if (ic_ui_hit(tz_button_rect(app, 0), x, y)) st.hover_tz = 0;
        else if (ic_ui_hit(tz_button_rect(app, 1), x, y)) st.hover_tz = 1;
    }
    if (st.pane == PANE_APPEARANCE) {
        for (int i = 0; i < IC_ACCENT_COUNT; i++) {
            if (ic_ui_hit(ic_rect_inset(swatch_rect(app, i), -4, -4), x, y)) st.hover_swatch = i;
        }
        st.hover_segment = ic_ui_segmented_hit(segmented_rect(app), 2, x, y);
    }
}

static void select_pane(int pane) {
    if (pane < 0 || pane >= PANE_COUNT || pane == st.pane) return;
    st.pane = pane;
    st.hover_row = -1;
    for (int i = 0; i < 4; i++) ic_tween_set(&st.row_hover[i], 0.0f);
}

static void set_appearance(int light) {
    if (st.s.appearance == light) return;
    st.s.appearance = light;
    ic_tween_to(&st.segment_pos, (float)light, IC_DUR_BASE, IC_EASE_MOVE);
    save();
    ic_palette_reload();
}

static void set_accent(int accent) {
    if (st.s.accent == accent) return;
    st.s.accent = accent;
    save();
    ic_palette_reload();
}

static void flip_toggle(int t) {
    int *v = toggle_value(t);
    *v = !*v;
    ic_tween_to(&st.toggle_pos[t], *v ? 1.0f : 0.0f, IC_DUR_BASE, IC_EASE_MOVE);
    if (t == TOG_AUDIO && !*v) icda_stop_audio();
    save();
}

static void click(ic_app_t *app, int x, int y) {
    for (int i = 0; i < PANE_COUNT; i++) {
        if (ic_ui_hit(sidebar_item_rect(i), x, y)) {
            select_pane(i);
            return;
        }
    }
    if (st.pane == PANE_TIME) {
        if (ic_ui_hit(tz_button_rect(app, 0), x, y)) set_tz(st.s.tz_minutes - 30);
        else if (ic_ui_hit(tz_button_rect(app, 1), x, y)) set_tz(st.s.tz_minutes + 30);
        return;
    }
    if (st.pane == PANE_APPEARANCE) {
        int seg = ic_ui_segmented_hit(segmented_rect(app), 2, x, y);
        if (seg >= 0) {
            set_appearance(seg);
            return;
        }
        for (int i = 0; i < IC_ACCENT_COUNT; i++) {
            if (ic_ui_hit(ic_rect_inset(swatch_rect(app, i), -4, -4), x, y)) {
                set_accent(i);
                return;
            }
        }
        return;
    }
    {
        ic_rect_t rows[4];
        int ids[TOG_COUNT];
        int n = current_rows(app, rows);
        pane_toggles(st.pane, ids);
        for (int i = 0; i < n; i++) {
            if (ic_ui_hit(rows[i], x, y)) {
                flip_toggle(ids[i]);
                return;
            }
        }
    }
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        update_hover(app, ev->x, ev->y);
        break;
    case IC_EV_MOUSE_LEAVE:
        update_hover(app, -1, -1);
        break;
    case IC_EV_MOUSE_DOWN:
        if (ev->button == GUI_BTN_LEFT) click(app, ev->x, ev->y);
        update_hover(app, ev->x, ev->y);
        break;
    case IC_EV_KEY:
        if (ev->key == IC_KEY_UP) select_pane(st.pane > 0 ? st.pane - 1 : 0);
        else if (ev->key == IC_KEY_DOWN) select_pane(st.pane < PANE_COUNT - 1 ? st.pane + 1 : st.pane);
        break;
    case IC_EV_APPEARANCE:
    case IC_EV_FOCUS:
        
        icda_settings_load(&st.s);
        ic_tween_set(&st.segment_pos, (float)st.s.appearance);
        for (int t = 0; t < TOG_COUNT; t++) ic_tween_set(&st.toggle_pos[t], *toggle_value(t) ? 1.0f : 0.0f);
        break;
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    (void)app;
    icda_settings_load(&st.s);
    st.pane = PANE_APPEARANCE;
    st.hover_pane = st.hover_row = st.hover_swatch = st.hover_segment = -1;
    ic_tween_set(&st.segment_pos, (float)st.s.appearance);
    for (int t = 0; t < TOG_COUNT; t++) ic_tween_set(&st.toggle_pos[t], *toggle_value(t) ? 1.0f : 0.0f);
    for (int i = 0; i < 4; i++) ic_tween_set(&st.row_hover[i], 0.0f);
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Settings", WIN_W, WIN_H, init, draw, event, 0 };
    (void)argc;
    (void)argv;
    if (ic_app_run(&desc, 0) != 0) {
        icda_write("settings requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
