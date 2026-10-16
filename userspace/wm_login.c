/* Sign-in, first account and lock screen (see wm_login.h). */
#include "wm_login.h"
#include "ic_ui.h"
#include "ic_time.h"

#define MAX_USERS  12
#define FIELD_CAP  128
#define CARD_W     340

typedef struct {
    char name[32];
    int  admin, automatic, password;
} user_t;

static int mode;
static user_t users[MAX_USERS];
static int nusers, sel;
/* SETUP: name, password, confirm; SIGNIN and LOCK: field 1 is the password */
static char field[3][FIELD_CAP];
static int flen[3], focus;
static char error[96];
static int caret_on = 1, hover = -1;
static uint64_t caret_ms;
static int clock_minute = -1;
static int esc_state;           /* ESC [ A: the arrow keys */

enum { HIT_NONE = -1, HIT_FIELD0 = 0, HIT_FIELD1, HIT_FIELD2, HIT_PRIMARY = 10, HIT_SECONDARY, HIT_USER0 = 20 };

static void wipe(void) {
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < FIELD_CAP; i++) ((volatile char *)field[f])[i] = 0;
        flen[f] = 0;
    }
}

static void load_users(void) {
    char buf[1024];
    long n = icda_account_list(buf, sizeof(buf));
    int i = 0;
    nusers = 0;
    if (n <= 0) return;
    while (i < n && nusers < MAX_USERS) {
        user_t *u = &users[nusers];
        int k = 0;
        while (i < n && buf[i] != ' ' && k < 31) u->name[k++] = buf[i++];
        u->name[k] = 0;
        while (i < n && buf[i] == ' ') i++;
        while (i < n && buf[i] != ' ') i++;                 /* uid */
        while (i < n && buf[i] == ' ') i++;
        u->admin = u->automatic = u->password = 0;
        for (; i < n && buf[i] != '\n'; i++) {
            if (buf[i] == 'a') u->admin = 1;
            if (buf[i] == 'l') u->automatic = 1;
            if (buf[i] == 'p') u->password = 1;
        }
        i++;
        if (u->name[0]) nusers++;
    }
}

static void load_me(void) {
    char buf[96];
    long n = icda_account_whoami(buf, sizeof(buf));
    nusers = 0;
    if (n <= 0) return;
    for (int i = 0, k = 0; i < n && buf[i] != ' ' && k < 31; i++) users[0].name[k++] = buf[i], users[0].name[k] = 0;
    users[0].password = 0;
    for (int i = 0; i < n; i++) if (buf[i] == 'p') users[0].password = 1;
    nusers = 1;
    sel = 0;
}

int wm_login_begin(int want) {
    wipe();
    error[0] = 0;
    sel = 0;
    focus = 1;
    hover = -1;
    esc_state = 0;
    caret_ms = ic_time_ms();
    if (want == WM_LOGIN_LOCK) {
        load_me();
        mode = WM_LOGIN_LOCK;
        return mode;
    }
    load_users();
    if (!nusers) {
        mode = WM_LOGIN_SETUP;
        focus = 0;
        return mode;
    }
    for (int i = 0; i < nusers; i++) {                      /* the one that signs in by itself */
        if (users[i].automatic && icda_account_login(users[i].name, 0) == 0) {
            mode = WM_LOGIN_NONE;
            return mode;
        }
    }
    mode = WM_LOGIN_SIGNIN;
    return mode;
}

void wm_login_end(void) {
    wipe();
}

/* ---- layout ------------------------------------------------------------------- */

static int user_has_password(void) {
    return nusers > 0 && users[sel].password;
}

static int nfields(void) {
    if (mode == WM_LOGIN_SETUP) return 3;
    return user_has_password() ? 1 : 0;
}

static int first_field(void) { return mode == WM_LOGIN_SETUP ? 0 : 1; }

static int card_h(void) {
    int h = 28;
    if (mode == WM_LOGIN_SETUP) h += 34 + 40 + 3 * 46 + 8;
    else h += (mode == WM_LOGIN_SIGNIN && nusers > 1 ? 92 : 0) + 72 + 30 + (user_has_password() ? 46 : 0);
    h += 40;                                   /* the main button */
    if (mode == WM_LOGIN_LOCK) h += 40;        /* sign out */
    h += 30;                                   /* the message line */
    return h;
}

static ic_rect_t card_rect(int sw, int sh) {
    int h = card_h(), y = sh * 36 / 100;
    if (y + h > sh - 24) y = sh - 24 - h;
    if (y < 120) y = 120;
    return ic_rect_make((sw - CARD_W) / 2, y, CARD_W, h);
}

/* where each part is; -1 width when absent */
static ic_rect_t part_rect(int sw, int sh, int id) {
    ic_rect_t c = card_rect(sw, sh);
    int x = c.x + 24, w = c.w - 48, y = c.y + 24;
    if (mode == WM_LOGIN_SETUP) {
        y += 34 + 40;
        for (int f = 0; f < 3; f++, y += 46) if (id == HIT_FIELD0 + f) return ic_rect_make(x, y, w, 36);
        y += 8;
    } else {
        if (mode == WM_LOGIN_SIGNIN && nusers > 1) {
            int shown = nusers < 5 ? nusers : 5, gap = 12, aw = 52;
            int total = shown * aw + (shown - 1) * gap, ax = c.x + (c.w - total) / 2;
            for (int i = 0; i < shown; i++)
                if (id == HIT_USER0 + i) return ic_rect_make(ax + i * (aw + gap), y, aw, 76);
            y += 92;
        }
        y += 72 + 30;                          /* the large initial and the name */
        if (user_has_password()) {
            if (id == HIT_FIELD1) return ic_rect_make(x, y, w, 36);
            y += 46;
        }
    }
    if (id == HIT_PRIMARY) return ic_rect_make(x, y, w, 34);
    y += 40;
    if (mode == WM_LOGIN_LOCK && id == HIT_SECONDARY) return ic_rect_make(x, y, w, 34);
    return ic_rect_make(0, 0, -1, 0);
}

static int hit(int sw, int sh, int mx, int my) {
    static const int ids[] = { HIT_FIELD0, HIT_FIELD1, HIT_FIELD2, HIT_PRIMARY, HIT_SECONDARY,
                               HIT_USER0, HIT_USER0 + 1, HIT_USER0 + 2, HIT_USER0 + 3, HIT_USER0 + 4 };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        ic_rect_t r = part_rect(sw, sh, ids[i]);
        if (r.w > 0 && ic_ui_hit(r, mx, my)) return ids[i];
    }
    return HIT_NONE;
}

/* ---- drawing ---------------------------------------------------------------------- */

static void avatar(ic_canvas_t *c, int cx, int cy, int r, const char *name, int selected) {
    const ic_palette_t *p = ic_palette();
    char initial[2] = { name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0 };
    const ic_face_t *f = ic_font(r > 30 ? IC_FONT_TITLE1 : IC_FONT_HEADLINE);
    if (selected) ic_gfx_circle(c, (float)cx, (float)cy, (float)r + 3.0f, p->focus_ring);
    ic_gfx_circle(c, (float)cx, (float)cy, (float)r, p->accent);
    ic_text_draw_in(c, f, ic_rect_make(cx - r, cy - r, 2 * r, 2 * r), initial, p->label_on_accent, IC_ALIGN_CENTER);
}

static void draw_field(ic_canvas_t *c, ic_rect_t r, int f, const char *placeholder, int secret) {
    char shown[FIELD_CAP];
    ic_textfield_t tf;
    int n = flen[f];
    for (int i = 0; i < n; i++) shown[i] = secret ? '*' : field[f][i];
    shown[n] = 0;
    tf.text = shown;
    tf.cursor = n;
    tf.sel_start = tf.sel_end = n;
    tf.focused = focus == f;
    tf.caret_on = caret_on;
    tf.placeholder = placeholder;
    tf.leading = secret ? IC_SYM_LOCK : IC_SYM_NONE;
    tf.scroll_px = 0;
    ic_ui_textfield_scroll(r, &tf);
    ic_ui_textfield(c, r, &tf);
}

static ic_state_t state_of(int id) {
    return ic_ui_state(1, hover == id, 0);
}

void wm_login_draw(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t card = card_rect(sw, sh), r;
    ic_datetime_t t;
    const ic_face_t *body = ic_font(IC_FONT_BODY);
    int y;

    /* the time, large, over the wallpaper */
    if (ic_wallclock(&t) == 0) {
        char hm[16], day[48];
        ic_format_hm(&t, hm, sizeof(hm));
        ic_format_day(&t, day, sizeof(day));
        ic_text_draw_in(c, ic_font(IC_FONT_LARGE_TITLE), ic_rect_make(0, card.y - 112, sw, 48), hm,
                        IC_WHITE, IC_ALIGN_CENTER);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(0, card.y - 62, sw, 22), day, IC_RGB(0xE8E8ED),
                        IC_ALIGN_CENTER);
    }

    ic_ui_panel(c, card, 16.0f, IC_ELEV_WINDOW, scratch, scratch_len);
    y = card.y + 24;
    if (mode == WM_LOGIN_SETUP) {
        ic_text_draw_in(c, ic_font(IC_FONT_TITLE2), ic_rect_make(card.x, y, card.w, 30), "Welcome to ICDA",
                        p->label, IC_ALIGN_CENTER);
        ic_text_draw_wrapped(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(card.x + 24, y + 34, card.w - 48, 36),
                             "Make the first account. It looks after this computer and keeps the files already here.",
                             p->label_secondary, 2);
        draw_field(c, part_rect(sw, sh, HIT_FIELD0), 0, "Name (lowercase)", 0);
        draw_field(c, part_rect(sw, sh, HIT_FIELD1), 1, "Password (optional)", 1);
        draw_field(c, part_rect(sw, sh, HIT_FIELD2), 2, "Password again", 1);
        ic_ui_button(c, part_rect(sw, sh, HIT_PRIMARY), "Create account", IC_SYM_NONE, IC_BUTTON_PRIMARY,
                     state_of(HIT_PRIMARY));
    } else {
        if (mode == WM_LOGIN_SIGNIN && nusers > 1) {
            int shown = nusers < 5 ? nusers : 5;
            for (int i = 0; i < shown; i++) {
                r = part_rect(sw, sh, HIT_USER0 + i);
                if (hover == HIT_USER0 + i && i != sel) ic_gfx_rrect(c, r.x - 4, r.y - 4, r.w + 8, r.h + 8, 10.0f, p->fill_hover);
                avatar(c, r.x + r.w / 2, r.y + 26, 24, users[i].name, i == sel);
                ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(r.x - 8, r.y + 56, r.w + 16, 16),
                                users[i].name, i == sel ? p->label : p->label_secondary, IC_ALIGN_CENTER);
            }
            y += 92;
        }
        if (nusers > 0) {
            avatar(c, card.x + card.w / 2, y + 32, 32, users[sel].name, 0);
            ic_text_draw_in(c, ic_font(IC_FONT_TITLE3), ic_rect_make(card.x, y + 72, card.w, 26), users[sel].name,
                            p->label, IC_ALIGN_CENTER);
        }
        if (user_has_password()) draw_field(c, part_rect(sw, sh, HIT_FIELD1), 1, "Password", 1);
        ic_ui_button(c, part_rect(sw, sh, HIT_PRIMARY), mode == WM_LOGIN_LOCK ? "Unlock" : "Sign in",
                     IC_SYM_NONE, IC_BUTTON_PRIMARY, state_of(HIT_PRIMARY));
        if (mode == WM_LOGIN_LOCK)
            ic_ui_button(c, part_rect(sw, sh, HIT_SECONDARY), "Sign out", IC_SYM_POWER, IC_BUTTON_PLAIN,
                         state_of(HIT_SECONDARY));
    }
    if (error[0]) {
        ic_text_draw_in(c, body, ic_rect_make(card.x + 16, card.y + card.h - 30, card.w - 32, 20), error,
                        p->danger, IC_ALIGN_CENTER);
    }
}

/* ---- acting --------------------------------------------------------------------- */

static void set_error(const char *s) { error[0] = 0; ic_strlcat(error, s, sizeof(error)); }

static int submit(void) {
    long rc;
    if (mode == WM_LOGIN_SETUP) {
        if (!flen[0]) { set_error("Choose a name"); focus = 0; return WM_LOGIN_REDRAW; }
        if (flen[1] != flen[2] || ic_strcmp(field[1], field[2]) != 0) {
            set_error("The passwords are not the same");
            focus = 2;
            return WM_LOGIN_REDRAW;
        }
        rc = icda_account_create(field[0], field[1], 0, 0);
        if (rc < 0) {
            set_error(rc == -17 ? "That name is taken" : rc == -22 ? "Use letters, digits, - and _" : "Could not make the account");
            return WM_LOGIN_REDRAW;
        }
        if (icda_account_login(field[0], field[1]) != 0) {
            set_error("Could not sign in");
            return WM_LOGIN_REDRAW;
        }
        wipe();
        return WM_LOGIN_DONE;
    }
    if (nusers <= 0) return WM_LOGIN_KEEP;
    set_error("");
    rc = mode == WM_LOGIN_LOCK ? icda_account_verify(field[1]) : icda_account_login(users[sel].name, field[1]);
    if (rc == 0) {
        wipe();
        return WM_LOGIN_DONE;
    }
    set_error("That password is not right");
    for (int i = 0; i < FIELD_CAP; i++) ((volatile char *)field[1])[i] = 0;
    flen[1] = 0;
    focus = 1;
    return WM_LOGIN_REDRAW;
}

static void select_user(int i) {
    if (i < 0 || i >= nusers || i >= 5 || i == sel) return;
    sel = i;
    error[0] = 0;
    for (int k = 0; k < FIELD_CAP; k++) ((volatile char *)field[1])[k] = 0;
    flen[1] = 0;
}

int wm_login_key(long key) {
    int f = focus;
    caret_on = 1;
    caret_ms = ic_time_ms();
    if (esc_state == 1) { esc_state = key == '[' ? 2 : 0; if (esc_state) return WM_LOGIN_KEEP; }
    else if (esc_state == 2) {
        esc_state = 0;
        if (mode == WM_LOGIN_SIGNIN && (key == 'C' || key == 'B')) { select_user(sel + 1); return WM_LOGIN_REDRAW; }
        if (mode == WM_LOGIN_SIGNIN && (key == 'D' || key == 'A')) { select_user(sel - 1); return WM_LOGIN_REDRAW; }
        return WM_LOGIN_KEEP;
    }
    if (key == 27) {
        esc_state = 1;
        if (f >= 0 && f < 3 && flen[f]) {
            for (int i = 0; i < FIELD_CAP; i++) ((volatile char *)field[f])[i] = 0;
            flen[f] = 0;
            return WM_LOGIN_REDRAW;
        }
        return WM_LOGIN_KEEP;
    }
    if (key == '\n' || key == '\r') {
        if (mode == WM_LOGIN_SETUP && focus < 2) { focus++; return WM_LOGIN_REDRAW; }
        return submit();
    }
    if (key == '\t') {
        if (nfields() > 1) focus = first_field() + (focus - first_field() + 1) % nfields();
        return WM_LOGIN_REDRAW;
    }
    if (f < 0 || f > 2 || !nfields()) return WM_LOGIN_KEEP;
    if (key == 8 || key == 127) {
        if (flen[f]) field[f][--flen[f]] = 0;
        return WM_LOGIN_REDRAW;
    }
    if (key < 32 || key > 126 || flen[f] >= FIELD_CAP - 1) return WM_LOGIN_KEEP;
    if (mode == WM_LOGIN_SETUP && f == 0) {                     /* names: a-z 0-9 - _ */
        if (key >= 'A' && key <= 'Z') key += 32;
        if (!((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9') || key == '-' || key == '_') || flen[0] >= 30)
            return WM_LOGIN_KEEP;
    }
    field[f][flen[f]++] = (char)key;
    field[f][flen[f]] = 0;
    return WM_LOGIN_REDRAW;
}

int wm_login_click(int sw, int sh, int x, int y) {
    int h = hit(sw, sh, x, y);
    if (h >= HIT_FIELD0 && h <= HIT_FIELD2) { focus = h; return WM_LOGIN_REDRAW; }
    if (h == HIT_PRIMARY) return submit();
    if (h == HIT_SECONDARY) return WM_LOGIN_SIGN_OUT;
    if (h >= HIT_USER0) { select_user(h - HIT_USER0); return WM_LOGIN_REDRAW; }
    return WM_LOGIN_KEEP;
}

int wm_login_hover(int sw, int sh, int x, int y) {
    int h = hit(sw, sh, x, y);
    if (h == hover) return WM_LOGIN_KEEP;
    hover = h;
    return WM_LOGIN_REDRAW;
}

int wm_login_tick(void) {
    uint64_t now = ic_time_ms();
    ic_datetime_t t;
    int r = WM_LOGIN_KEEP;
    if (now - caret_ms >= 530) {
        caret_ms = now;
        caret_on = !caret_on;
        r = WM_LOGIN_REDRAW;
    }
    if (ic_wallclock(&t) == 0 && t.minute != clock_minute) {
        clock_minute = t.minute;
        r = WM_LOGIN_REDRAW;
    }
    return r;
}
