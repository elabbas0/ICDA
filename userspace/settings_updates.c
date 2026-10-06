/*
 * Settings > Updates.  The updater daemon (/sbin/updated) does the work;
 * this page shows what it reports through /dev/sysupdate and asks it to
 * check now.  Patches install while restarting, so the page offers that.
 */

#include "settings_updates.h"

#define DEV        "/dev/sysupdate"
#define RELEASE    "/etc/icda-release.txt"
#define POLL_MS    700
#define ROW_H      IC_H_ROW_TALL

static struct {
    char     version[24];
    char     notes[160];
    char     status[128];
    char     pending[24];
    char     bad[24];
    char     rolledback[24];
    int      supported;
    int      checking;
    uint32_t last_poll;
    int      hover;        /* 0 check, 1 restart */
} u;

static void field(const char *r, const char *key, char *out, int cap) {
    int klen = (int)ic_strlen(key);
    out[0] = 0;
    for (int i = 0; r[i]; ) {
        int s = i, e;
        while (r[i] && r[i] != '\n') i++;
        e = i;
        if (r[i]) i++;
        if (e - s > klen && ic_memcmp(r + s, key, (uint64_t)klen) == 0 && r[s + klen] == ':') {
            int from = s + klen + 1, n;
            while (from < e && r[from] == ' ') from++;
            n = e - from < cap - 1 ? e - from : cap - 1;
            ic_memcpy(out, r + from, (uint64_t)n);
            out[n] = 0;
            return;
        }
    }
}

/* "version 1.6.0" / "notes ..." lines of the installed manifest */
static void release_field(const char *r, const char *key, char *out, int cap) {
    int klen = (int)ic_strlen(key);
    out[0] = 0;
    for (int i = 0; r[i]; ) {
        int s = i, e;
        while (r[i] && r[i] != '\n') i++;
        e = i;
        if (r[i]) i++;
        if (e - s > klen && ic_memcmp(r + s, key, (uint64_t)klen) == 0 && r[s + klen] == ' ') {
            int n = e - s - klen - 1 < cap - 1 ? e - s - klen - 1 : cap - 1;
            ic_memcpy(out, r + s + klen + 1, (uint64_t)n);
            out[n] = 0;
            return;
        }
    }
}

static void poll(int force) {
    static char r[1024];
    static char rel[2048];
    char tmp[8];
    uint32_t now = ic_time_ms();
    long n;
    if (!force && now - u.last_poll < POLL_MS) return;
    u.last_poll = now;
    n = (long)icda_read_file(DEV, r, sizeof(r) - 1);
    r[n > 0 ? n : 0] = 0;
    field(r, "status", u.status, sizeof(u.status));
    field(r, "pending", u.pending, sizeof(u.pending));
    if (ic_streq(u.pending, "none")) u.pending[0] = 0;
    field(r, "bad", u.bad, sizeof(u.bad));
    field(r, "rolledback", u.rolledback, sizeof(u.rolledback));
    field(r, "supported", tmp, sizeof(tmp));
    u.supported = tmp[0] == '1';
    field(r, "check", tmp, sizeof(tmp));
    u.checking = tmp[0] == '1';
    n = (long)icda_read_file(RELEASE, rel, sizeof(rel) - 1);
    rel[n > 0 ? n : 0] = 0;
    release_field(rel, "version", u.version, sizeof(u.version));
    release_field(rel, "notes", u.notes, sizeof(u.notes));
}

void updates_pane_enter(void) {
    u.hover = -1;
    poll(1);
}

void updates_pane_tick(ic_app_t *app) {
    char before[128 + 24];
    ic_strcpy(before, u.status, sizeof(before));
    ic_strlcat(before, u.pending, sizeof(before));
    poll(0);
    {
        char after[128 + 24];
        ic_strcpy(after, u.status, sizeof(after));
        ic_strlcat(after, u.pending, sizeof(after));
        if (!ic_streq(before, after)) ic_app_invalidate(app);
    }
}

/* ---- layout ------------------------------------------------------------- */

static ic_rect_t info_group(ic_rect_t a) {
    return ic_rect_make(a.x, a.y, a.w, 2 * ROW_H);
}

static ic_rect_t check_btn(ic_rect_t a) {
    ic_rect_t g = info_group(a);
    return ic_rect_make(a.x, g.y + g.h + IC_SP_4, 150, IC_H_CONTROL);
}

static ic_rect_t restart_btn(ic_rect_t a) {
    ic_rect_t b = check_btn(a);
    return ic_rect_make(b.x + b.w + IC_SP_3, b.y, 190, IC_H_CONTROL);
}

/* ---- drawing ------------------------------------------------------------ */

void updates_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t a) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t g = info_group(a), r;
    char line[200];
    int y;
    (void)app;

    ic_ui_group(c, g);
    r = ic_rect_make(g.x, g.y, g.w, ROW_H);
    ic_ui_group_row(c, r, 0, 2, 0.0f);
    line[0] = 0;
    ic_strlcat(line, "ICDA ", sizeof(line));
    ic_strlcat(line, u.version[0] ? u.version : "(development build)", sizeof(line));
    ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(r.x + IC_SP_3, r.y + 8, r.w - 2 * IC_SP_3, 18),
                    line, p->label, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + IC_SP_3, r.y + 28, r.w - 2 * IC_SP_3, 16),
                    u.notes[0] ? u.notes : "Installed version", p->label_secondary, IC_ALIGN_LEFT);

    r.y += ROW_H;
    ic_ui_group_row(c, r, 1, 2, 0.0f);
    if (!u.supported) {
        ic_strcpy(line, "Over-the-air updates need an installed ICDA", sizeof(line));
    } else if (u.pending[0]) {
        line[0] = 0;
        ic_strlcat(line, "Update ", sizeof(line));
        ic_strlcat(line, u.pending, sizeof(line));
        ic_strlcat(line, " is ready: restart to install it", sizeof(line));
    } else {
        ic_strcpy(line, u.status[0] ? u.status : "Waiting for the updater", sizeof(line));
    }
    ic_symbol_draw(c, u.pending[0] ? IC_SYM_CHECK : IC_SYM_RELOAD, (float)(r.x + IC_SP_3 + 8),
                   (float)(r.y + r.h / 2), 16.0f, u.pending[0] ? p->accent : p->label_secondary);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r.x + IC_SP_3 + 26, r.y, r.w - 2 * IC_SP_3 - 26, r.h),
                    line, p->label, IC_ALIGN_LEFT);

    ic_ui_button(c, check_btn(a), u.checking ? "Checking..." : "Check now", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 !u.supported || u.checking ? IC_STATE_DISABLED :
                 u.hover == 0 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    if (u.pending[0]) {
        ic_ui_button(c, restart_btn(a), "Restart and install", IC_SYM_NONE, IC_BUTTON_PRIMARY,
                     u.hover == 1 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    }

    y = check_btn(a).y + IC_H_CONTROL + IC_SP_5;
    if (u.rolledback[0]) {
        line[0] = 0;
        ic_strlcat(line, "Update ", sizeof(line));
        ic_strlcat(line, u.rolledback, sizeof(line));
        ic_strlcat(line, " did not start correctly and was undone.", sizeof(line));
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(a.x, y, a.w, 16), line, p->danger,
                        IC_ALIGN_LEFT);
        y += 22;
    }
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(a.x, y, a.w, 16),
                    "ICDA checks for updates every few hours and downloads only what changed.",
                    p->label_secondary, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(a.x, y + 18, a.w, 16),
                    "Updates install while restarting and never touch your files or settings.",
                    p->label_secondary, IC_ALIGN_LEFT);
}

/* ---- input -------------------------------------------------------------- */

int updates_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t a) {
    (void)app;
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
    case IC_EV_MOUSE_LEAVE: {
        int h = ic_ui_hit(check_btn(a), ev->x, ev->y) ? 0 :
                (u.pending[0] && ic_ui_hit(restart_btn(a), ev->x, ev->y)) ? 1 : -1;
        if (h == u.hover) return 0;
        u.hover = h;
        return 1;
    }
    case IC_EV_MOUSE_DOWN:
        if (u.supported && !u.checking && ic_ui_hit(check_btn(a), ev->x, ev->y)) {
            (void)icda_write_file(DEV, "check", 5);
            poll(1);
            return 1;
        }
        if (u.pending[0] && ic_ui_hit(restart_btn(a), ev->x, ev->y)) {
            (void)icda_power(1);
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}
