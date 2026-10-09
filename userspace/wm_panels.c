/* Taskbar volume, battery and notifications: the quick settings flyout, the
 * notification centre and toasts.  State comes from the kernel - /dev/battery,
 * /dev/power, /dev/notify and the mixer's master volume - and the volume and
 * power mode are kept in the settings file. */

#include "wm_panels.h"
#include "wm_shell.h"
#include "settings_store.h"

#define FLY_W        340
#define NOTE_W       360
#define FLY_PAD      IC_SP_4
#define FLY_GAP      8
#define QUICK_H      262
#define NOTE_ROW_H   62
#define NOTE_ROWS    6
#define NOTE_HEAD_H  52
#define TOAST_W      340
#define TOAST_H      76
#define TOAST_MS     5000
#define MAX_NOTES    50

typedef struct {
    uint32_t seq, at_s;
    char app[24], title[64], body[160];
} note_t;

static struct {
    int present, ac, percent, state, minutes;      /* state: 0 idle/full, 1 discharging, 2 charging */
    int volume, muted;
    int power, power_ok;
    note_t notes[MAX_NOTES];
    int nnotes;
    uint32_t seen_seq, read_seq;
    int toast;                      /* index into notes, -1 none */
    uint64_t toast_until;
    int dragging;
    int hover;                      /* hovered control in a panel */
    int warned;                     /* low battery warnings given: 1 at 20 %, 2 at 10 % */
    uint64_t last_poll;
} P = { .toast = -1, .volume = 70 };

static uint64_t now_ms(void) { return icda_ticks() * 10; }

/* ---- kernel state ---------------------------------------------------------------- */

static int field(const char *text, const char *key, int fallback) {
    int klen = (int)ic_strlen(key);
    for (const char *l = text; *l; ) {
        if (ic_memcmp(l, key, (uint64_t)klen) == 0 && l[klen] == ' ') {
            int v = 0, neg = 0;
            const char *p = l + klen + 1;
            if (*p == '-') { neg = 1; p++; }
            while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            return neg ? -v : v;
        }
        while (*l && *l != '\n') l++;
        if (*l) l++;
    }
    return fallback;
}

static int has_word(const char *text, const char *word) {
    int n = (int)ic_strlen(word);
    for (const char *p = text; *p; p++) if (ic_memcmp(p, word, (uint64_t)n) == 0) return 1;
    return 0;
}

static void apply_volume(void) {
    (void)icda_audio_master_set((uint32_t)(P.volume * 256 / 100), P.muted);
}

static void apply_power(void) {
    static const char *const cmd[] = { "mode saver", "mode balanced", "mode performance" };
    (void)icda_write_file("/dev/power", cmd[P.power], ic_strlen(cmd[P.power]));
}

static void save_settings(void) {
    icda_settings_t s;
    icda_settings_load(&s);
    s.volume = P.volume;
    s.muted = P.muted;
    s.power = P.power;
    (void)icda_settings_save(&s);
}

static void post(const char *title, const char *body) {
    char msg[256];
    msg[0] = 0;
    ic_strlcat(msg, "System|", sizeof(msg));
    ic_strlcat(msg, title, sizeof(msg));
    ic_strlcat(msg, "\n", sizeof(msg));
    ic_strlcat(msg, body, sizeof(msg));
    (void)icda_write_file("/dev/notify", msg, ic_strlen(msg));
}

void wm_panels_init(void) {
    icda_settings_t s;
    char r[256];
    long n;
    icda_settings_load(&s);
    P.volume = s.volume;
    P.muted = s.muted;
    P.power = s.power;
    apply_volume();
    n = (long)icda_read_file("/dev/power", r, sizeof(r) - 1);
    if (n > 0) {
        r[n] = 0;
        P.power_ok = field(r, "supported", 0);
    }
    apply_power();
    /* notifications already in the store are history, not news */
    n = (long)icda_read_file("/dev/notify", r, 2);
    (void)n;
    P.seen_seq = 0xFFFFFFFFu;
}

static int read_battery(void) {
    char r[512];
    long n = (long)icda_read_file("/dev/battery", r, sizeof(r) - 1);
    int before = P.present * 100000 + P.ac * 10000 + P.percent * 10 + P.state;
    if (n <= 0) return 0;
    r[n] = 0;
    P.present = field(r, "present", 0);
    P.ac = field(r, "ac", 1);
    P.percent = field(r, "percent", 0);
    P.minutes = field(r, "minutes", -1);
    P.state = has_word(r, "state charging") ? 2 : has_word(r, "state discharging") ? 1 : 0;
    if (P.present && P.state == 1) {
        if (P.percent <= 10 && P.warned < 2) {
            P.warned = 2;
            post("Battery very low", "10% left. Plug in your computer now.");
        } else if (P.percent <= 20 && P.warned < 1) {
            P.warned = 1;
            post("Battery low", "20% left. Plug in your computer soon.");
        }
    } else if (P.ac) {
        P.warned = 0;
    }
    return before != P.present * 100000 + P.ac * 10000 + P.percent * 10 + P.state;
}

/* "seq\tat\tapp\ttitle\tbody\n" per notification, oldest first */
static int read_notes(void) {
    static char r[MAX_NOTES * 300];
    long n = (long)icda_read_file("/dev/notify", r, sizeof(r) - 1);
    const char *p = r;
    uint32_t newest = 0;
    int fresh = -1;
    if (n < 0) n = 0;
    r[n] = 0;
    P.nnotes = 0;
    while (*p && P.nnotes < MAX_NOTES) {
        note_t *e = &P.notes[P.nnotes];
        char *dst[3] = { e->app, e->title, e->body };
        int cap[3] = { (int)sizeof(e->app), (int)sizeof(e->title), (int)sizeof(e->body) };
        e->seq = 0;
        while (*p >= '0' && *p <= '9') e->seq = e->seq * 10 + (uint32_t)(*p++ - '0');
        if (*p == '\t') p++;
        e->at_s = 0;
        while (*p >= '0' && *p <= '9') e->at_s = e->at_s * 10 + (uint32_t)(*p++ - '0');
        for (int f = 0; f < 3; f++) {
            int k = 0;
            if (*p == '\t') p++;
            while (*p && *p != '\t' && *p != '\n') {
                if (k + 1 < cap[f]) dst[f][k++] = *p;
                p++;
            }
            dst[f][k] = 0;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
        if (e->seq > newest) newest = e->seq;
        if (P.seen_seq != 0xFFFFFFFFu && e->seq > P.seen_seq) fresh = P.nnotes;
        P.nnotes++;
    }
    if (P.seen_seq == 0xFFFFFFFFu) {                 /* first read: nothing is new */
        P.seen_seq = P.read_seq = newest;
        return 1;
    }
    if (fresh >= 0) {
        P.seen_seq = newest;
        P.toast = fresh;
        P.toast_until = now_ms() + TOAST_MS;
        ic_sound("notify");
        return 1;
    }
    if (P.nnotes == 0 && P.read_seq) P.read_seq = 0;
    return 0;
}

int wm_panels_poll(uint32_t min_ms) {
    int changed = 0;
    uint64_t t = now_ms();
    if (P.toast >= 0 && t >= P.toast_until) {
        P.toast = -1;
        changed |= 4;
    }
    if (P.last_poll && t - P.last_poll < min_ms) return changed;
    P.last_poll = t;
    if (read_battery()) changed |= 3;
    if (read_notes()) changed |= 7;
    return changed;
}

int wm_panels_battery(int *percent, int *charging) {
    if (percent) *percent = P.percent;
    if (charging) *charging = P.ac;
    return P.present;
}

int wm_panels_volume(int *muted) {
    if (muted) *muted = P.muted;
    return P.volume;
}

int wm_panels_unread(void) {
    int n = 0;
    for (int i = 0; i < P.nnotes; i++) if (P.notes[i].seq > P.read_seq) n++;
    return n;
}

/* ---- layout -------------------------------------------------------------------------- */

ic_rect_t wm_panels_rect(int which, int sw, int sh) {
    if (which == WM_PANEL_NOTES) {
        int rows = P.nnotes < NOTE_ROWS ? P.nnotes : NOTE_ROWS;
        int h = NOTE_HEAD_H + (rows ? rows * NOTE_ROW_H : 70) + IC_SP_3;
        return ic_rect_make(sw - NOTE_W - FLY_GAP, sh - WM_BAR_H - FLY_GAP - h, NOTE_W, h);
    }
    return ic_rect_make(sw - FLY_W - FLY_GAP, sh - WM_BAR_H - FLY_GAP - QUICK_H, FLY_W, QUICK_H);
}

static ic_rect_t mute_btn(ic_rect_t f) { return ic_rect_make(f.x + FLY_PAD, f.y + 58, 36, 36); }
static ic_rect_t vol_slider(ic_rect_t f) { return ic_rect_make(f.x + FLY_PAD + 48, f.y + 64, f.w - 2 * FLY_PAD - 48 - 44, 24); }
static ic_rect_t power_seg(ic_rect_t f) { return ic_rect_make(f.x + FLY_PAD, f.y + 136, f.w - 2 * FLY_PAD, IC_H_CONTROL); }
static ic_rect_t clear_btn(ic_rect_t f) { return ic_rect_make(f.x + f.w - FLY_PAD - 84, f.y + 12, 84, IC_H_CONTROL); }

/* ---- drawing -------------------------------------------------------------------------- */

static void battery_text(char *out, int cap) {
    char num[12];
    out[0] = 0;
    if (!P.present) {
        ic_strlcat(out, "No battery", (uint64_t)cap);
        return;
    }
    ic_uint_to_str((uint64_t)P.percent, num, sizeof(num));
    ic_strlcat(out, num, (uint64_t)cap);
    ic_strlcat(out, "%  -  ", (uint64_t)cap);
    ic_strlcat(out, P.state == 2 ? "Charging" : P.state == 1 ? "On battery" : P.ac ? "Plugged in" : "Idle",
               (uint64_t)cap);
    if (P.minutes > 0) {
        ic_strlcat(out, ", ", (uint64_t)cap);
        if (P.minutes >= 60) {
            ic_uint_to_str((uint64_t)(P.minutes / 60), num, sizeof(num));
            ic_strlcat(out, num, (uint64_t)cap);
            ic_strlcat(out, " h ", (uint64_t)cap);
        }
        ic_uint_to_str((uint64_t)(P.minutes % 60), num, sizeof(num));
        ic_strlcat(out, num, (uint64_t)cap);
        ic_strlcat(out, P.state == 2 ? " min to full" : " min left", (uint64_t)cap);
    }
}

/* the battery glyph, also used by the taskbar */
void wm_battery_glyph(ic_canvas_t *c, float cx, float cy, int percent, int charging, ic_color_t tint,
                      ic_color_t fill) {
    int w = 22, h = 12, x = (int)cx - w / 2 - 1, y = (int)cy - h / 2;
    int inner = (w - 4) * (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100;
    ic_gfx_rrect(c, x, y, w, h, 3.0f, tint);
    ic_gfx_rrect(c, x + 1, y + 1, w - 2, h - 2, 2.0f, ic_palette()->material_bar);
    if (inner > 0) ic_gfx_rrect(c, x + 2, y + 2, inner, h - 4, 1.5f, fill);
    ic_gfx_rrect(c, x + w, y + 3, 2, h - 6, 1.0f, tint);
    if (charging) {
        float bx = cx - 1.0f, by = cy;
        ic_gfx_line(c, bx + 2.0f, by - 5.0f, bx - 2.0f, by + 1.0f, 1.6f, ic_palette()->label);
        ic_gfx_line(c, bx - 2.0f, by + 1.0f, bx + 2.0f, by + 1.0f, 1.6f, ic_palette()->label);
        ic_gfx_line(c, bx + 2.0f, by + 1.0f, bx - 2.0f, by + 6.0f, 1.6f, ic_palette()->label);
    }
}

/* the bell glyph for the notification button */
void wm_bell_glyph(ic_canvas_t *c, float cx, float cy, ic_color_t tint) {
    ic_gfx_rrect(c, (int)cx - 6, (int)cy - 7, 12, 12, 6.0f, tint);
    ic_gfx_rrect(c, (int)cx - 8, (int)cy + 2, 16, 3, 1.5f, tint);
    ic_gfx_rrect(c, (int)cx - 2, (int)cy + 6, 4, 3, 1.5f, tint);
    ic_gfx_rrect(c, (int)cx - 1, (int)cy - 9, 2, 3, 1.0f, tint);
}

static void draw_quick(ic_canvas_t *c, ic_rect_t f, uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p = ic_palette();
    static const char *const modes[] = { "Saver", "Balanced", "Performance" };
    char line[96], num[8];
    ic_ui_panel(c, f, IC_R_PANEL, IC_ELEV_MENU, scratch, scratch_len);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(f.x + FLY_PAD, f.y + 16, f.w - 2 * FLY_PAD, 18),
                    "Quick settings", p->label, IC_ALIGN_LEFT);

    /* volume */
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(f.x + FLY_PAD, f.y + 40, 120, 14), "Volume",
                    p->label_secondary, IC_ALIGN_LEFT);
    {
        ic_rect_t mb = mute_btn(f);
        ic_ui_icon_button(c, mb, IC_SYM_SPEAKER, P.hover == 1 ? IC_STATE_HOVER : IC_STATE_NORMAL);
        if (P.muted) ic_gfx_line(c, (float)mb.x + 9.0f, (float)(mb.y + mb.h) - 9.0f, (float)(mb.x + mb.w) - 9.0f,
                                 (float)mb.y + 9.0f, 1.8f, p->label);
    }
    ic_ui_slider(c, vol_slider(f), P.muted ? 0.0f : (float)P.volume / 100.0f,
                 P.dragging ? IC_STATE_PRESSED : P.hover == 2 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_uint_to_str((uint64_t)(P.muted ? 0 : P.volume), num, sizeof(num));
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(f.x + f.w - FLY_PAD - 36, f.y + 68, 36, 16), num,
                    p->label_secondary, IC_ALIGN_RIGHT);

    /* power mode */
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(f.x + FLY_PAD, f.y + 114, f.w - 2 * FLY_PAD, 14),
                    P.power_ok ? "Power mode" : "Power mode (not supported by this processor)",
                    p->label_secondary, IC_ALIGN_LEFT);
    ic_ui_segmented(c, power_seg(f), modes, 3, (float)P.power, P.power_ok && P.hover >= 10 ? P.hover - 10 : -1);

    /* battery */
    ic_gfx_hline(c, f.x + FLY_PAD, f.y + 190, f.w - 2 * FLY_PAD, p->separator);
    wm_battery_glyph(c, (float)(f.x + FLY_PAD + 12), (float)(f.y + 222), P.present ? P.percent : 0, P.ac && P.present,
                     p->label_secondary, P.present && P.percent <= 20 && !P.ac ? p->danger : p->label);
    battery_text(line, sizeof(line));
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(f.x + FLY_PAD + 34, f.y + 213, f.w - 2 * FLY_PAD - 34, 18),
                    line, p->label, IC_ALIGN_LEFT);
}

static void time_ago(uint32_t at_s, char *out, int cap) {
    uint32_t now = (uint32_t)(icda_ticks() / 100), d = now > at_s ? now - at_s : 0;
    char num[12];
    out[0] = 0;
    if (d < 60) { ic_strlcat(out, "now", (uint64_t)cap); return; }
    ic_uint_to_str(d < 3600 ? d / 60 : d / 3600, num, sizeof(num));
    ic_strlcat(out, num, (uint64_t)cap);
    ic_strlcat(out, d < 3600 ? " min ago" : " h ago", (uint64_t)cap);
}

static void draw_note(ic_canvas_t *c, ic_rect_t r, const note_t *e) {
    const ic_palette_t *p = ic_palette();
    char ago[24];
    ic_rect_t saved;
    ic_canvas_push_clip(c, r.x, r.y, r.w, r.h, &saved);
    ic_gfx_rrect(c, r.x + 2, r.y + 14, 28, 28, 8.0f, ic_color_with_alpha(p->accent, 0x30));
    wm_bell_glyph(c, (float)(r.x + 16), (float)(r.y + 28), p->accent);
    time_ago(e->at_s, ago, sizeof(ago));
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(r.x + r.w - 80, r.y + 8, 80, 14), ago,
                    p->label_tertiary, IC_ALIGN_RIGHT);
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(r.x + 40, r.y + 8, r.w - 130, 14),
                    e->app[0] ? e->app : "Notification", p->label_secondary, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(r.x + 40, r.y + 24, r.w - 44, 18), e->title,
                    p->label, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + 40, r.y + 42, r.w - 44, 16), e->body,
                    p->label_secondary, IC_ALIGN_LEFT);
    ic_canvas_pop_clip(c, &saved);
}

static void draw_notes(ic_canvas_t *c, ic_rect_t f, uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p = ic_palette();
    int rows = P.nnotes < NOTE_ROWS ? P.nnotes : NOTE_ROWS;
    ic_ui_panel(c, f, IC_R_PANEL, IC_ELEV_MENU, scratch, scratch_len);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(f.x + FLY_PAD, f.y + 18, 200, 18), "Notifications",
                    p->label, IC_ALIGN_LEFT);
    if (P.nnotes) ic_ui_button(c, clear_btn(f), "Clear all", IC_SYM_NONE, IC_BUTTON_PLAIN,
                               P.hover == 20 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_gfx_hline(c, f.x + FLY_PAD, f.y + NOTE_HEAD_H - 4, f.w - 2 * FLY_PAD, p->separator);
    if (!rows) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(f.x, f.y + NOTE_HEAD_H + 20, f.w, 18),
                        "No notifications", p->label_tertiary, IC_ALIGN_CENTER);
        return;
    }
    for (int i = 0; i < rows; i++) {           /* newest first */
        const note_t *e = &P.notes[P.nnotes - 1 - i];
        draw_note(c, ic_rect_make(f.x + FLY_PAD, f.y + NOTE_HEAD_H + i * NOTE_ROW_H, f.w - 2 * FLY_PAD, NOTE_ROW_H), e);
        if (i + 1 < rows) ic_gfx_hline(c, f.x + FLY_PAD + 40, f.y + NOTE_HEAD_H + (i + 1) * NOTE_ROW_H - 1,
                                       f.w - 2 * FLY_PAD - 40, p->separator);
    }
}

void wm_panels_draw(int which, ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len) {
    ic_rect_t f = wm_panels_rect(which, sw, sh);
    if (which == WM_PANEL_NOTES) draw_notes(c, f, scratch, scratch_len);
    else draw_quick(c, f, scratch, scratch_len);
}

/* ---- input ------------------------------------------------------------------------------ */

void wm_panels_open(int which) {
    P.hover = 0;
    P.dragging = 0;
    if (which == WM_PANEL_NOTES) {
        uint32_t newest = 0;
        for (int i = 0; i < P.nnotes; i++) if (P.notes[i].seq > newest) newest = P.notes[i].seq;
        P.read_seq = newest;
        P.toast = -1;
    } else {
        (void)read_battery();
    }
}

static void set_volume_at(ic_rect_t f, int mx) {
    int v = (int)(ic_ui_slider_value(vol_slider(f), mx) * 100.0f + 0.5f);
    P.volume = v < 0 ? 0 : v > 100 ? 100 : v;
    P.muted = 0;
    apply_volume();
}

int wm_panels_click(int which, int sw, int sh, int mx, int my) {
    ic_rect_t f = wm_panels_rect(which, sw, sh);
    if (which == WM_PANEL_NOTES) {
        if (P.nnotes && ic_ui_hit(clear_btn(f), mx, my)) {
            (void)icda_write_file("/dev/notify", "clear", 5);
            P.nnotes = 0;
            P.read_seq = 0;
            return WM_PANEL_REDRAW;
        }
        return WM_PANEL_KEEP;
    }
    if (ic_ui_hit(mute_btn(f), mx, my)) {
        P.muted = !P.muted;
        apply_volume();
        save_settings();
        if (!P.muted) ic_sound_always("sound_on");
        return WM_PANEL_REDRAW;
    }
    if (ic_ui_hit(ic_rect_make(vol_slider(f).x - 8, vol_slider(f).y - 8, vol_slider(f).w + 16, vol_slider(f).h + 16),
                  mx, my)) {
        P.dragging = 1;
        set_volume_at(f, mx);
        return WM_PANEL_REDRAW;
    }
    if (P.power_ok) {
        int seg = ic_ui_segmented_hit(power_seg(f), 3, mx, my);
        if (seg >= 0 && seg != P.power) {
            P.power = seg;
            apply_power();
            save_settings();
            ic_sound("toggle_on");
            return WM_PANEL_REDRAW;
        }
    }
    return WM_PANEL_KEEP;
}

int wm_panels_drag(int which, int sw, int sh, int mx, int my) {
    (void)my;
    if (!P.dragging || which != WM_PANEL_QUICK) return WM_PANEL_KEEP;
    set_volume_at(wm_panels_rect(which, sw, sh), mx);
    return WM_PANEL_REDRAW;
}

int wm_panels_release(void) {
    if (!P.dragging) return WM_PANEL_KEEP;
    P.dragging = 0;
    save_settings();
    ic_sound_always("sound_on");          /* hear the new level */
    return WM_PANEL_REDRAW;
}

int wm_panels_hover(int which, int sw, int sh, int mx, int my) {
    ic_rect_t f = wm_panels_rect(which, sw, sh);
    int h = 0;
    if (which == WM_PANEL_NOTES) {
        if (P.nnotes && ic_ui_hit(clear_btn(f), mx, my)) h = 20;
    } else {
        int seg = ic_ui_segmented_hit(power_seg(f), 3, mx, my);
        if (ic_ui_hit(mute_btn(f), mx, my)) h = 1;
        else if (ic_ui_hit(vol_slider(f), mx, my)) h = 2;
        else if (seg >= 0) h = 10 + seg;
    }
    if (h == P.hover) return WM_PANEL_KEEP;
    P.hover = h;
    return WM_PANEL_REDRAW;
}

int wm_panels_wheel(int delta) {
    int v = P.volume + (delta > 0 ? 5 : -5);
    P.volume = v < 0 ? 0 : v > 100 ? 100 : v;
    P.muted = 0;
    apply_volume();
    save_settings();
    return WM_PANEL_REDRAW;
}

/* ---- toast ----------------------------------------------------------------------------------- */

int wm_toast_visible(void) { return P.toast >= 0 && P.toast < P.nnotes; }

ic_rect_t wm_toast_rect(int sw, int sh) {
    return ic_rect_make(sw - TOAST_W - FLY_GAP, sh - WM_BAR_H - FLY_GAP - TOAST_H, TOAST_W, TOAST_H);
}

void wm_toast_draw(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len) {
    ic_rect_t f = wm_toast_rect(sw, sh);
    if (!wm_toast_visible()) return;
    ic_ui_panel(c, f, IC_R_PANEL, IC_ELEV_MENU, scratch, scratch_len);
    draw_note(c, ic_rect_make(f.x + IC_SP_3, f.y + 6, f.w - 2 * IC_SP_3, TOAST_H - 8), &P.notes[P.toast]);
}

void wm_toast_dismiss(void) { P.toast = -1; }
