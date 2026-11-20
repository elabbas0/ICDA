/* Dialogs: what programs and the kernel ask for through /dev/dialog (the
 * format is in kernel/dev/dialog.c).  The window manager is the store's
 * host: it reads the open requests, draws them, and writes each answer
 * back; a program that asked is also sent GUI_MSG_DIALOG when its dialog is
 * answered, so it need not wait for its next look.
 *
 * One look: an icon for the kind, the title, the message, and the buttons
 * in a row on the right, the default one filled.  Enter or Space presses the
 * focused button (at first the default), Escape the cancel one, Tab and the
 * arrow keys move between them. */

#include "wm_dialog.h"
#include "ic_font.h"
#include "ic_theme.h"
#include "libicda.h"
#include "icda_sys.h"

#define WD_MAX        16
#define WD_W          440
#define WD_PAD        20
#define WD_ICON       36
#define WD_GAP        14
#define WD_BTN_GAP    8
#define WD_BTN_MIN    84
#define WD_MSG_LINES  14
#define WD_POLL_TICKS 25            /* 250 ms */
#define WD_ESC_TICKS  3             /* a lone Escape: no '[' within 30 ms */
#define WD_SHADOW     28            /* how far the shadow reaches */

enum { WD_INFO = 0, WD_WARNING, WD_ERROR, WD_QUESTION };

typedef struct {
    int      used;
    uint32_t id;                    /* the store's; 0 for the window manager's own */
    uint32_t window;                /* the window it sits on; 0 the screen */
    int      kind;
    char     title[160];
    char     message[1200];
    char     buttons[4][48];
    int      nbuttons, def, cancel, destructive;
    int      focus;                 /* what Enter and Space press */
    int      keyboard;              /* focus moved by keys: its ring shows */
    int      hover, pressed;
    int      seen;
    uint32_t order;                 /* arrival: the screen's show oldest first */
    uint32_t stopped;               /* the kernel's: the program that stopped */
    char     label[40];             /* the kernel's: the name it gave that program */
} wd_t;

static wd_t wd[WD_MAX];
static uint32_t order_next = 1;
static uint32_t last_gen;
static uint64_t last_poll;
static int hosting;
static int (*window_exists)(uint32_t id);
static wm_dialog_answered_fn answered_fn;
static wm_dialog_stopped_fn stopped_fn;
static uint32_t last_answered_window;

/* keys: Escape waits to see whether it starts a sequence (an arrow key) */
static int esc_pending, seq_state;
static uint32_t esc_window;
static uint64_t esc_tick;

/* ---- the store ------------------------------------------------------------------------ */

static void copy_n(char *dst, int cap, const char *src, int n) {
    int i = 0;
    for (; i < n && i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static long to_int(const char *s, int n) {
    long v = 0;
    int i = 0, neg = 0;
    while (i < n && s[i] == ' ') i++;
    if (i < n && s[i] == '-') { neg = 1; i++; }
    while (i < n && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
    return neg ? -v : v;
}

static wd_t *by_id(uint32_t id) {
    for (int i = 0; i < WD_MAX; i++)
        if (wd[i].used && wd[i].id == id && id) return &wd[i];
    return 0;
}

static wd_t *free_slot(void) {
    for (int i = 0; i < WD_MAX; i++)
        if (!wd[i].used) return &wd[i];
    return 0;
}

/* the dialog shown for a window (0: the oldest of the screen's) */
static wd_t *shown(uint32_t window_id) {
    wd_t *best = 0;
    for (int i = 0; i < WD_MAX; i++) {
        wd_t *d = &wd[i];
        if (!d->used || d->window != window_id) continue;
        if (!best || d->order < best->order) best = d;
    }
    return best;
}

static void set_buttons(wd_t *d, const char *s, int n) {
    int i = 0;
    d->nbuttons = 0;
    while (i < n && d->nbuttons < 4) {
        int start = i, end;
        while (i < n && s[i] != '|') i++;
        end = i;
        while (start < end && s[start] == ' ') start++;
        while (end > start && s[end - 1] == ' ') end--;
        if (end > start) copy_n(d->buttons[d->nbuttons++], sizeof(d->buttons[0]), s + start, end - start);
        i++;
    }
}

static void finish(wd_t *d) {
    if (!d->nbuttons) {
        copy_n(d->buttons[0], sizeof(d->buttons[0]), "OK", 2);
        d->nbuttons = 1;
    }
    if (d->def < 0 || d->def >= d->nbuttons) d->def = 0;
    if (d->cancel < 0 || d->cancel >= d->nbuttons) d->cancel = d->nbuttons - 1;
    if (d->destructive >= d->nbuttons) d->destructive = -1;
    d->focus = d->def;
    d->hover = d->pressed = -1;
    d->keyboard = 0;
    if (d->window && window_exists && !window_exists(d->window)) d->window = 0;
    if (!d->title[0]) copy_n(d->title, sizeof(d->title), "A program asks", 14);
}

/* one request: its lines up to an empty one; returns where the next starts */
static const char *parse_one(const char *s, wd_t *d, uint32_t *id) {
    int msg = 0;
    *id = 0;
    d->kind = WD_INFO;
    d->title[0] = d->message[0] = 0;
    d->nbuttons = 0;
    d->def = 0;
    d->cancel = -1;
    d->destructive = -1;
    d->window = 0;
    d->stopped = 0;
    d->label[0] = 0;
    while (*s && *s != '\n') {
        const char *eol = s, *eq;
        int klen, vlen;
        while (*eol && *eol != '\n') eol++;
        eq = s;
        while (eq < eol && *eq != '=') eq++;
        if (eq < eol) {
            const char *v = eq + 1;
            klen = (int)(eq - s);
            vlen = (int)(eol - v);
#define KEY(k) (klen == (int)sizeof(k) - 1 && ic_memcmp(s, k, sizeof(k) - 1) == 0)
            if (KEY("id")) *id = (uint32_t)to_int(v, vlen);
            else if (KEY("kind")) d->kind = vlen >= 4 && ic_memcmp(v, "warn", 4) == 0 ? WD_WARNING
                                           : vlen >= 5 && ic_memcmp(v, "error", 5) == 0 ? WD_ERROR
                                           : vlen >= 8 && ic_memcmp(v, "question", 8) == 0 ? WD_QUESTION : WD_INFO;
            else if (KEY("title")) copy_n(d->title, sizeof(d->title), v, vlen);
            else if (KEY("message")) {
                /* each message= line is a line of the message */
                if (msg && msg + 1 < (int)sizeof(d->message)) d->message[msg++] = '\n';
                for (int k = 0; k < vlen && msg + 1 < (int)sizeof(d->message); k++) d->message[msg++] = v[k];
                d->message[msg] = 0;
            }
            else if (KEY("buttons")) set_buttons(d, v, vlen);
            else if (KEY("default")) d->def = (int)to_int(v, vlen);
            else if (KEY("cancel")) d->cancel = (int)to_int(v, vlen);
            else if (KEY("destructive")) d->destructive = (int)to_int(v, vlen);
            else if (KEY("window")) d->window = (uint32_t)to_int(v, vlen);
            else if (KEY("stopped")) d->stopped = (uint32_t)to_int(v, vlen);
            else if (KEY("label")) copy_n(d->label, sizeof(d->label), v, vlen);
#undef KEY
        }
        s = *eol ? eol + 1 : eol;
    }
    return *s ? s + 1 : s;
}

void wm_dialog_init(int (*exists)(uint32_t window_id)) {
    window_exists = exists;
    hosting = (long)icda_write_file("/dev/dialog", "host", 4) == 4;
    last_poll = 0;
}

int wm_dialog_poll(int now, uint32_t *focus_window) {
    static char buf[24576];
    uint64_t t = icda_ticks();
    long n;
    int changed = 0;
    const char *s;
    if (focus_window) *focus_window = 0;
    if (!now && t - last_poll < WD_POLL_TICKS) return 0;
    last_poll = t;
    if (!hosting) hosting = (long)icda_write_file("/dev/dialog", "host", 4) == 4;
    if (!hosting) return 0;
    n = (long)icda_read_file("/dev/dialog", buf, sizeof(buf) - 1);
    if (n <= 0) return 0;
    buf[n] = 0;
    if (!ic_strprefix(buf, "gen=")) {
        hosting = 0;                 /* the store no longer takes us for its host */
        return 0;
    }
    {
        uint32_t gen = (uint32_t)to_int(buf + 4, 12);
        if (gen == last_gen) return 0;
        last_gen = gen;
    }
    for (int i = 0; i < WD_MAX; i++) wd[i].seen = 0;
    s = buf;
    while (*s && *s != '\n') s++;     /* gen= */
    if (*s) s++;
    if (*s == '\n') s++;
    while (*s) {
        static wd_t tmp;
        uint32_t id;
        wd_t *d;
        if (*s == '\n') { s++; continue; }
        s = parse_one(s, &tmp, &id);
        if (!id) continue;
        d = by_id(id);
        if (d) { d->seen = 1; continue; }
        d = free_slot();
        if (!d) continue;
        *d = tmp;
        d->id = id;
        d->used = 1;
        d->seen = 1;
        d->order = order_next++;
        if (d->stopped && stopped_fn) {
            /* the program as its window called it: "Activity", not "Taskman" */
            char name[40];
            int ll = (int)ic_strlen(d->label);
            if (stopped_fn(d->stopped, name, sizeof(name)) && name[0] && ll && ic_strprefix(d->title, d->label)) {
                char t[sizeof(d->title)];
                t[0] = 0;
                ic_strlcat(t, name, sizeof(t));
                ic_strlcat(t, d->title + ll, sizeof(t));
                copy_n(d->title, sizeof(d->title), t, sizeof(d->title) - 1);
            }
        }
        finish(d);
        changed = 1;
        if (focus_window && d->window) *focus_window = d->window;
    }
    /* the store's that left it: answered elsewhere, taken back, or their program ended */
    for (int i = 0; i < WD_MAX; i++) {
        if (wd[i].used && wd[i].id && !wd[i].seen) {
            if (esc_pending && esc_window == wd[i].window) esc_pending = 0;
            wd[i].used = 0;
            changed = 1;
        }
    }
    return changed;
}

int wm_dialog_has(uint32_t window_id) {
    return shown(window_id) != 0;
}

void wm_dialog_local(const char *kind, const char *title, const char *message) {
    wd_t *d = free_slot();
    if (!d) return;
    ic_memzero(d, sizeof(*d));
    d->used = 1;
    d->order = order_next++;
    d->kind = kind && ic_streq(kind, "error") ? WD_ERROR : kind && ic_streq(kind, "warning") ? WD_WARNING : WD_INFO;
    copy_n(d->title, sizeof(d->title), title ? title : "", 159);
    copy_n(d->message, sizeof(d->message), message ? message : "", 1199);
    d->cancel = -1;
    d->destructive = -1;
    finish(d);
}

void wm_dialog_on_answer(wm_dialog_answered_fn fn) {
    answered_fn = fn;
}

void wm_dialog_on_stopped(wm_dialog_stopped_fn fn) {
    stopped_fn = fn;
}

static void answer(wd_t *d, int button) {
    if (d->id) {
        char msg[48] = "answer ";
        char num[16];
        int k = 0;
        uint32_t v = d->id;
        do num[k++] = (char)('0' + v % 10); while (v /= 10);
        {
            int n = (int)ic_strlen(msg);
            while (k) msg[n++] = num[--k];
            msg[n++] = ' ';
            if (button < 0) { msg[n++] = '-'; msg[n++] = '1'; }
            else msg[n++] = (char)('0' + button);
            msg[n] = 0;
        }
        (void)icda_write_file("/dev/dialog", msg, ic_strlen(msg));
    }
    last_answered_window = d->window;
    if (answered_fn && d->window && d->id) answered_fn(d->window, d->id, button);
    if (esc_pending && esc_window == d->window) esc_pending = 0;
    d->used = 0;
}

void wm_dialog_window_gone(uint32_t window_id) {
    if (!window_id) return;
    for (int i = 0; i < WD_MAX; i++)
        if (wd[i].used && wd[i].window == window_id) answer(&wd[i], -1);
}

uint32_t wm_dialog_last_window(void) {
    return last_answered_window;
}

/* ---- layout and drawing ---------------------------------------------------------------- */

typedef struct {
    ic_rect_t r, icon, title, message, btn[4];
} wd_layout_t;

static int text_w(void) {
    return WD_W - 2 * WD_PAD - WD_ICON - WD_GAP;
}

static int msg_cap(void) {
    return WD_MSG_LINES * (ic_font(IC_FONT_BODY)->line_h + 3);
}

static void layout(const wd_t *d, ic_rect_t content, int sw, int sh, wd_layout_t *L) {
    int tw = text_w(), w = WD_W, h, th, mh, body, x, y, bx, total = 0;
    th = ic_text_draw_wrapped(0, ic_font(IC_FONT_HEADLINE), ic_rect_make(0, 0, tw, 0), d->title, 0, 2);
    mh = d->message[0] ? ic_text_draw_wrapped(0, ic_font(IC_FONT_BODY), ic_rect_make(0, 0, tw, msg_cap()), d->message, 0, 3) : 0;
    body = th + (mh ? 6 + mh : 0);
    if (body < WD_ICON) body = WD_ICON;
    if (w > sw - 16) w = sw - 16;
    h = WD_PAD + body + WD_PAD + IC_H_CONTROL + WD_PAD;
    if (d->window) {
        /* over the window's content, a third of the way down, never off the screen */
        x = content.x + (content.w - w) / 2;
        y = content.h > h + 48 ? content.y + (content.h - h) / 3 : content.y + (content.h - h) / 2;
    } else {
        x = (sw - w) / 2;
        y = (sh - h) * 2 / 5;
    }
    if (x + w > sw - 8) x = sw - 8 - w;
    if (y + h > sh - 8) y = sh - 8 - h;
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    L->r = ic_rect_make(x, y, w, h);
    L->icon = ic_rect_make(x + WD_PAD, y + WD_PAD, WD_ICON, WD_ICON);
    L->title = ic_rect_make(x + WD_PAD + WD_ICON + WD_GAP, y + WD_PAD, tw, th);
    L->message = ic_rect_make(L->title.x, y + WD_PAD + th + 6, tw, mh);
    for (int i = 0; i < d->nbuttons; i++) {
        int bw = ic_ui_button_width(d->buttons[i], IC_SYM_NONE);
        if (bw < WD_BTN_MIN) bw = WD_BTN_MIN;
        L->btn[i] = ic_rect_make(0, y + h - WD_PAD - IC_H_CONTROL, bw, IC_H_CONTROL);
        total += bw + (i ? WD_BTN_GAP : 0);
    }
    if (total > w - 2 * WD_PAD) {
        /* labels too long for one row at their width: equal shares */
        int bw = (w - 2 * WD_PAD - WD_BTN_GAP * (d->nbuttons - 1)) / d->nbuttons;
        for (int i = 0; i < d->nbuttons; i++) L->btn[i].w = bw;
    }
    bx = x + w - WD_PAD;
    for (int i = d->nbuttons - 1; i >= 0; i--) {
        bx -= L->btn[i].w;
        L->btn[i].x = bx;
        bx -= WD_BTN_GAP;
    }
}

ic_rect_t wm_dialog_rect(uint32_t window_id, ic_rect_t content, int sw, int sh) {
    wd_layout_t L;
    const wd_t *d = shown(window_id);
    if (!d) return ic_rect_make(0, 0, 0, 0);
    layout(d, content, sw, sh, &L);
    return L.r;
}

ic_rect_t wm_dialog_reach(ic_rect_t r) {
    return ic_rect_make(r.x - WD_SHADOW, r.y - WD_SHADOW, r.w + 2 * WD_SHADOW, r.h + 2 * WD_SHADOW);
}

static void draw_icon(ic_canvas_t *c, int kind, ic_rect_t s) {
    const ic_palette_t *p = ic_palette();
    float cx = (float)s.x + (float)s.w / 2.0f, cy = (float)s.y + (float)s.h / 2.0f;
    switch (kind) {
    case WD_ERROR:
        ic_gfx_rrect(c, s.x, s.y, s.w, s.h, (float)s.w / 2.0f, p->danger);
        ic_symbol_draw(c, IC_SYM_CLOSE, cx, cy, (float)s.w * 0.55f, IC_WHITE);
        break;
    case WD_WARNING:
        ic_symbol_draw(c, IC_SYM_WARNING, cx, cy, (float)s.w, p->warning);
        break;
    case WD_QUESTION:
        ic_gfx_rrect(c, s.x, s.y, s.w, s.h, (float)s.w / 2.0f, p->accent);
        ic_text_draw_in(c, ic_font(IC_FONT_TITLE2), s, "?", IC_WHITE, IC_ALIGN_CENTER);
        break;
    default:
        ic_symbol_draw(c, IC_SYM_INFO, cx, cy, (float)s.w, p->accent);
        break;
    }
}

void wm_dialog_draw(ic_canvas_t *c, uint32_t window_id, ic_rect_t content, float corner, int sw, int sh) {
    const ic_palette_t *p = ic_palette();
    const wd_t *d = shown(window_id);
    wd_layout_t L;
    if (!d) return;
    layout(d, content, sw, sh, &L);
    if (window_id) {
        /* the window waits: its content dims */
        ic_gfx_rrect4(c, content.x, content.y, content.w, content.h, 0.0f, 0.0f, corner, corner, IC_BLACK_A(0x38));
    }
    ic_theme_shadow(c, L.r.x, L.r.y, L.r.w, L.r.h, IC_R_PANEL, IC_ELEV_WINDOW);
    ic_gfx_rrect(c, L.r.x, L.r.y, L.r.w, L.r.h, IC_R_PANEL, ic_color_over(p->window, p->material_menu));
    ic_gfx_rrect_stroke(c, L.r.x, L.r.y, L.r.w, L.r.h, IC_R_PANEL, 1.0f, p->frame);
    draw_icon(c, d->kind, L.icon);
    ic_text_draw_wrapped(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(L.title.x, L.title.y, L.title.w, 0), d->title, p->label, 2);
    if (d->message[0])
        ic_text_draw_wrapped(c, ic_font(IC_FONT_BODY), ic_rect_make(L.message.x, L.message.y, L.message.w, msg_cap()),
                             d->message, p->label_secondary, 3);
    for (int i = 0; i < d->nbuttons; i++) {
        ic_button_style_t style = i == d->destructive ? IC_BUTTON_DESTRUCTIVE
                                : i == d->def ? IC_BUTTON_PRIMARY : IC_BUTTON_DEFAULT;
        ic_state_t state = d->pressed == i && d->hover == i ? IC_STATE_PRESSED
                         : d->hover == i ? IC_STATE_HOVER : IC_STATE_NORMAL;
        ic_ui_button(c, L.btn[i], d->buttons[i], IC_SYM_NONE, style, state);
        if (d->keyboard && d->focus == i) ic_ui_focus_ring(c, L.btn[i], IC_R_CONTROL);
    }
}

/* ---- input ---------------------------------------------------------------------------- */

static int button_at(const wd_t *d, ic_rect_t r, int x, int y) {
    wd_layout_t L;
    /* the layout is relative to where the dialog is: lay it out at r */
    layout(d, ic_rect_make(0, 0, 0, 0), 1 << 20, 1 << 20, &L);
    for (int i = 0; i < d->nbuttons; i++) {
        ic_rect_t b = L.btn[i];
        b.x += r.x - L.r.x;
        b.y += r.y - L.r.y;
        if (ic_ui_hit(b, x, y)) return i;
    }
    return -1;
}

int wm_dialog_hover(uint32_t window_id, ic_rect_t r, int x, int y) {
    wd_t *d = shown(window_id);
    int b;
    if (!d) return 0;
    b = ic_ui_hit(r, x, y) ? button_at(d, r, x, y) : -1;
    if (b == d->hover) return 0;
    d->hover = b;
    return 1;
}

int wm_dialog_press(uint32_t window_id, ic_rect_t r, int x, int y) {
    wd_t *d = shown(window_id);
    int b;
    if (!d) return 0;
    b = button_at(d, r, x, y);
    if (b < 0) return 0;
    d->pressed = d->hover = b;
    d->keyboard = 0;
    return 1;
}

int wm_dialog_release(uint32_t window_id, ic_rect_t r, int x, int y) {
    wd_t *d = shown(window_id);
    int b, was;
    if (!d || d->pressed < 0) return 0;
    b = button_at(d, r, x, y);
    was = d->pressed;
    d->pressed = -1;
    if (b == was) {
        answer(d, was);
        return 2;
    }
    return 1;
}

int wm_dialog_key(uint32_t window_id, long key) {
    wd_t *d = shown(window_id);
    if (!d) return 0;
    if (seq_state == 1) {
        /* ESC [ x: parameters until the final byte */
        if (key >= '0' && key <= '9') return 0;
        if (key == ';') return 0;
        seq_state = 0;
        if (key == 'C' || key == 'B') { d->focus = (d->focus + 1) % d->nbuttons; d->keyboard = 1; return 1; }
        if (key == 'D' || key == 'A' || key == 'Z') { d->focus = (d->focus + d->nbuttons - 1) % d->nbuttons; d->keyboard = 1; return 1; }
        return 0;
    }
    if (esc_pending) {
        esc_pending = 0;
        if (key == '[') { seq_state = 1; return 0; }
        answer(d, d->cancel);         /* Escape, then something else */
        return 2;
    }
    if (key == 27) {
        esc_pending = 1;
        esc_window = window_id;
        esc_tick = icda_ticks();
        return 0;
    }
    if (key == 13 || key == 10 || key == ' ') {
        answer(d, d->focus);
        return 2;
    }
    if (key == 9) {
        d->focus = (d->focus + 1) % d->nbuttons;
        d->keyboard = 1;
        return 1;
    }
    return 0;
}

int wm_dialog_tick(void) {
    wd_t *d;
    if (!esc_pending || icda_ticks() - esc_tick < WD_ESC_TICKS) return 0;
    esc_pending = 0;
    d = shown(esc_window);
    if (!d) return 0;
    answer(d, d->cancel);
    return 2;
}
