/*
 * Settings > Wi-Fi: network list, password sheet, connection status and
 * the driver log, on top of /dev/wifi.
 */

#include "settings_wifi.h"

#define WIFI_DEV        "/dev/wifi"
#define NET_MAX         32
#define REPORT_CAP      8192
#define LOG_CAP         16384
#define POLL_MS         500
#define AUTOSCAN_MS     15000
#define ROW_H           IC_H_ROW_TALL
#define STATUS_H        IC_H_ROW_TALL
#define LIST_TOP        (STATUS_H + 48)

typedef struct {
    char ssid[33];
    char sec[12];
    int  pct;
    int  dbm;
    int  chan;
    int  saved;
} wifi_net_t;

static struct {
    /* last report */
    char        state[24];
    char        ssid[40];
    char        ip[20];
    char        signal[20];
    char        rate[20];
    char        error[100];
    char        adapter[64];
    char        mac[24];
    wifi_net_t  nets[NET_MAX];
    int         nnets;
    char        log[LOG_CAP];
    /* ui */
    int         show_log;
    int         scroll;
    int         hover_row;
    int         hover_forget;
    int         hover_btn;      /* 0 primary, 1 log */
    char        notice[100];
    uint32_t    last_poll;
    uint32_t    last_scan;
    /* password sheet */
    int         sheet;
    char        sheet_ssid[33];
    char        pass[65];
    int         reveal;
    int         sheet_hover;    /* 0 cancel, 1 join */
} w;

/* ---- /dev/wifi ---------------------------------------------------------- */

static int wifi_cmd(const char *cmd) {
    return icda_write_file(WIFI_DEV, cmd, ic_strlen(cmd)) == (uint64_t)-1 ? -1 : 0;
}

static void copy_value(char *dst, int cap, const char *src, int len) {
    int n = len < cap - 1 ? len : cap - 1;
    ic_memcpy(dst, src, (uint64_t)n);
    dst[n] = 0;
}

static int parse_int(const char *s, int *pos) {
    int v = 0, neg = 0, i = *pos;
    if (s[i] == '-') { neg = 1; i++; }
    while (s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
    *pos = i;
    return neg ? -v : v;
}

/* "key: value" lines, then "--- networks" and TAB-separated rows. */
static void parse_report(const char *r) {
    int i = 0, in_nets = 0;

    w.state[0] = w.ssid[0] = w.ip[0] = w.signal[0] = w.rate[0] = 0;
    w.error[0] = w.adapter[0] = w.mac[0] = 0;
    w.nnets = 0;
    while (r[i]) {
        int s = i, e;
        while (r[i] && r[i] != '\n') i++;
        e = i;
        if (r[i]) i++;
        if (e - s >= 3 && r[s] == '-' && r[s + 1] == '-' && r[s + 2] == '-') {
            in_nets = 1;
            continue;
        }
        if (in_nets) {
            wifi_net_t *n;
            int p = s, f, k;
            if (w.nnets >= NET_MAX) continue;
            n = &w.nets[w.nnets];
            n->pct = parse_int(r, &p); if (r[p] == '\t') p++;
            n->dbm = parse_int(r, &p); if (r[p] == '\t') p++;
            n->chan = parse_int(r, &p); if (r[p] == '\t') p++;
            for (f = p; f < e && r[f] != '\t'; f++) {}
            copy_value(n->sec, sizeof(n->sec), r + p, f - p);
            p = f + 1;
            for (f = p; f < e && r[f] != '\t'; f++) {}
            n->saved = (f - p == 5 && r[p] == 's');
            p = f + 1;
            if (p > e) continue;
            copy_value(n->ssid, sizeof(n->ssid), r + p, e - p);
            for (k = 0; n->ssid[k]; k++) {}
            if (k == 0) continue;
            w.nnets++;
            continue;
        }
        {
            int c = s;
            while (c < e && r[c] != ':') c++;
            if (c >= e || c + 2 > e) continue;
#define FIELD(name, dst) \
            if (c - s == (int)sizeof(name) - 1 && ic_memcmp(r + s, name, sizeof(name) - 1) == 0) \
                copy_value(dst, sizeof(dst), r + c + 2, e - c - 2)
            FIELD("state", w.state);
            FIELD("ssid", w.ssid);
            FIELD("ip", w.ip);
            FIELD("signal", w.signal);
            FIELD("rate", w.rate);
            FIELD("error", w.error);
            FIELD("adapter", w.adapter);
            FIELD("mac", w.mac);
#undef FIELD
        }
    }
}

static void poll(int force) {
    static char report[REPORT_CAP];
    uint32_t now = ic_time_ms();

    if (!force && now - w.last_poll < POLL_MS) return;
    w.last_poll = now;
    if (w.show_log) {
        if (wifi_cmd("log") == 0)
            (void)icda_read_file(WIFI_DEV, w.log, sizeof(w.log));
    }
    if (wifi_cmd("all") != 0) {
        ic_strcpy(w.state, "no-adapter", sizeof(w.state));
        w.nnets = 0;
        return;
    }
    if ((long)icda_read_file(WIFI_DEV, report, sizeof(report)) < 0) report[0] = 0;
    parse_report(report);
}

static void int_to_str(int v, char *out, uint64_t cap) {
    if (cap < 2) return;
    if (v < 0) {
        out[0] = '-';
        ic_uint_to_str((uint64_t)(-v), out + 1, cap - 1);
    } else
        ic_uint_to_str((uint64_t)v, out, cap);
}

static int is_connected(void) { return ic_streq(w.state, "connected") || ic_streq(w.state, "no-ip") || ic_streq(w.state, "dhcp"); }

static int is_connecting(void) {
    return ic_streq(w.state, "searching") || ic_streq(w.state, "authenticating") ||
           ic_streq(w.state, "associating") || ic_streq(w.state, "handshake");
}

static int running(void) {
    return !ic_streq(w.state, "no-adapter") && !ic_streq(w.state, "starting") &&
           !ic_streq(w.state, "error") && !ic_streq(w.state, "radio-off") && w.state[0];
}

static void do_scan(void) {
    if (wifi_cmd("scan") == 0) w.last_scan = ic_time_ms();
    poll(1);
}

static void connect_to(const char *ssid, const char *pass) {
    char cmd[128];
    cmd[0] = 0;
    ic_strlcat(cmd, "connect\t", sizeof(cmd));
    ic_strlcat(cmd, ssid, sizeof(cmd));
    ic_strlcat(cmd, "\t", sizeof(cmd));
    ic_strlcat(cmd, pass, sizeof(cmd));
    w.notice[0] = 0;
    if (wifi_cmd(cmd) != 0) ic_strcpy(w.notice, "The Wi-Fi driver rejected the request.", sizeof(w.notice));
    ic_memzero(cmd, sizeof(cmd));
    poll(1);
}

static void forget(const char *ssid) {
    char cmd[64];
    cmd[0] = 0;
    ic_strlcat(cmd, "forget\t", sizeof(cmd));
    ic_strlcat(cmd, ssid, sizeof(cmd));
    (void)wifi_cmd(cmd);
    poll(1);
}

void wifi_pane_enter(void) {
    w.hover_row = w.hover_forget = w.hover_btn = -1;
    w.scroll = 0;
    poll(1);
    if (running() && !is_connected() && !is_connecting()) do_scan();
}

void wifi_pane_tick(ic_app_t *app) {
    char before[24 + 40 + 20 + 100];
    int n = w.nnets;
    before[0] = 0;
    ic_strlcat(before, w.state, sizeof(before));
    ic_strlcat(before, w.ssid, sizeof(before));
    ic_strlcat(before, w.ip, sizeof(before));
    ic_strlcat(before, w.error, sizeof(before));
    poll(0);
    if (running() && !is_connected() && !is_connecting() && !w.sheet &&
        ic_time_ms() - w.last_scan > AUTOSCAN_MS)
        do_scan();
    {
        char after[sizeof(before)];
        after[0] = 0;
        ic_strlcat(after, w.state, sizeof(after));
        ic_strlcat(after, w.ssid, sizeof(after));
        ic_strlcat(after, w.ip, sizeof(after));
        ic_strlcat(after, w.error, sizeof(after));
        if (!ic_streq(before, after) || n != w.nnets || w.show_log) ic_app_invalidate(app);
    }
}

int wifi_pane_modal(void) { return w.sheet; }

/* ---- layout ------------------------------------------------------------- */

static ic_rect_t status_rect(ic_rect_t a) { return ic_rect_make(a.x, a.y, a.w, STATUS_H); }

static ic_rect_t primary_btn_rect(ic_rect_t a) {
    ic_rect_t s = status_rect(a);
    const char *label = (is_connected() || is_connecting()) ? "Disconnect" : "Scan";
    int bw = ic_ui_button_width(label, IC_SYM_NONE);
    return ic_rect_make(s.x + s.w - IC_SP_3 - bw, s.y + (s.h - IC_H_CONTROL_SM) / 2, bw, IC_H_CONTROL_SM);
}

static int running(void);

static ic_rect_t log_btn_rect(ic_rect_t a) {
    ic_rect_t p = primary_btn_rect(a);
    const char *label = w.show_log ? "Networks" : "Log";
    int bw = ic_ui_button_width(label, IC_SYM_NONE);
    if (!running()) return ic_rect_make(p.x + p.w - bw, p.y, bw, p.h);
    return ic_rect_make(p.x - IC_SP_2 - bw, p.y, bw, p.h);
}

static ic_rect_t list_rect(ic_rect_t a) {
    return ic_rect_make(a.x, a.y + LIST_TOP, a.w, a.h - LIST_TOP);
}

static int visible_rows(ic_rect_t a) {
    int n = list_rect(a).h / ROW_H;
    return n < 1 ? 1 : n;
}

static ic_rect_t net_row_rect(ic_rect_t a, int i) {
    ic_rect_t l = list_rect(a);
    int count = w.nnets - w.scroll;
    int vis = visible_rows(a);
    if (count > vis) count = vis;
    return ic_rect_make(l.x, l.y + i * ROW_H, l.w, ROW_H);
}

static ic_rect_t forget_rect(ic_rect_t row) {
    return ic_rect_make(row.x + row.w - IC_SP_3 - IC_H_CONTROL_SM, row.y + (row.h - IC_H_CONTROL_SM) / 2,
                        IC_H_CONTROL_SM, IC_H_CONTROL_SM);
}

static ic_rect_t sheet_rect(ic_app_t *app) {
    int sw = 420, sh = 168;
    return ic_rect_make((app->width - sw) / 2, (app->height - sh) / 2, sw, sh);
}

static ic_rect_t sheet_field(ic_rect_t r) {
    return ic_rect_make(r.x + IC_SP_4, r.y + 64, r.w - 2 * IC_SP_4, IC_H_CONTROL);
}

static ic_rect_t sheet_btn(ic_rect_t r, int which) {
    int bj = ic_ui_button_width("Join", IC_SYM_NONE), bc = ic_ui_button_width("Cancel", IC_SYM_NONE);
    int y = r.y + r.h - IC_SP_4 - IC_H_CONTROL;
    int xj = r.x + r.w - IC_SP_4 - bj;
    if (which == 1) return ic_rect_make(xj, y, bj, IC_H_CONTROL);
    return ic_rect_make(xj - IC_SP_2 - bc, y, bc, IC_H_CONTROL);
}

/* ---- drawing ------------------------------------------------------------ */

/* Cut `s' with "..." so it fits in max_w pixels of font `style'. */
static void ellipsize(char *s, uint64_t cap, ic_font_style_t style, int max_w) {
    const ic_face_t *f = ic_font(style);
    int n;
    if (max_w <= 0 || ic_text_measure(f, s) <= max_w) return;
    n = ic_text_fit(f, s, max_w - ic_text_measure(f, "..."));
    if (n < 0) n = 0;
    if ((uint64_t)n + 4 > cap) n = (int)cap - 4;
    s[n] = 0;
    ic_strlcat(s, "...", cap);
}

/* ic_ui_row_text() with the texts shortened to the row width. */
static void row_text_fit(ic_canvas_t *c, ic_rect_t r, ic_symbol_t sym, ic_color_t tint,
                         const char *title, const char *sub) {
    char t[96], s[160];
    int text_w = r.w - (IC_SP_3 + 24 + IC_SP_3 - 2) - IC_SP_2;
    ic_strcpy(t, title, sizeof(t));
    ic_strcpy(s, sub, sizeof(s));
    ellipsize(t, sizeof(t), IC_FONT_BODY, text_w);
    ellipsize(s, sizeof(s), IC_FONT_FOOTNOTE, text_w);
    ic_ui_row_text(c, r, sym, tint, t, s);
}

static ic_color_t signal_tint(int pct) {
    if (pct >= 60) return IC_RGB(0x30D158);
    if (pct >= 35) return IC_RGB(IC_TINT_ORANGE);
    return IC_RGB(0xFF453A);
}

static void status_text(char *title, int tcap, char *sub, int scap) {
    title[0] = sub[0] = 0;
    if (ic_streq(w.state, "no-adapter") || !w.state[0]) {
        ic_strlcat(title, "No Wi-Fi adapter", tcap);
        ic_strlcat(sub, "Supported: Intel Dual Band Wireless-AC 8260", scap);
    } else if (ic_streq(w.state, "starting")) {
        ic_strlcat(title, "Starting Wi-Fi...", tcap);
        ic_strlcat(sub, "Loading the firmware", scap);
    } else if (ic_streq(w.state, "error") || ic_streq(w.state, "radio-off")) {
        ic_strlcat(title, ic_streq(w.state, "radio-off") ? "Wi-Fi is off" : "Wi-Fi failed to start", tcap);
        ic_strlcat(sub, w.error[0] ? w.error : "See the log for details", scap);
    } else if (is_connected()) {
        ic_strlcat(title, "Connected to ", tcap);
        ic_strlcat(title, w.ssid, tcap);
        if (ic_streq(w.state, "dhcp")) ic_strlcat(sub, "Getting an IP address...", scap);
        else if (ic_streq(w.state, "no-ip")) ic_strlcat(sub, "No IP address (DHCP failed)", scap);
        else {
            ic_strlcat(sub, w.ip, scap);
            if (w.rate[0]) { ic_strlcat(sub, "  -  ", scap); ic_strlcat(sub, w.rate, scap); }
            if (w.signal[0]) { ic_strlcat(sub, "  -  ", scap); ic_strlcat(sub, w.signal, scap); }
        }
    } else if (is_connecting()) {
        ic_strlcat(title, ic_streq(w.state, "searching") ? "Looking for " : "Connecting to ", tcap);
        ic_strlcat(title, w.ssid, tcap);
        ic_strlcat(sub, ic_streq(w.state, "handshake") ? "Checking the password (WPA2 handshake)" :
                        ic_streq(w.state, "searching") ? "Scanning for the network" :
                        "Joining the access point", scap);
    } else {
        ic_strlcat(title, "Not connected", tcap);
        ic_strlcat(sub, w.error[0] ? w.error :
                        ic_streq(w.state, "scanning") ? "Scanning..." : "Choose a network below", scap);
    }
}

static void draw_status(ic_app_t *app, ic_canvas_t *c, ic_rect_t a) {
    ic_rect_t s = status_rect(a);
    ic_rect_t pb = primary_btn_rect(a), lb = log_btn_rect(a);
    char title[80], sub[140];
    (void)app;
    status_text(title, sizeof(title), sub, sizeof(sub));
    ic_ui_group(c, s);
    row_text_fit(c, ic_rect_make(s.x, s.y, lb.x - s.x - IC_SP_2, s.h), IC_SYM_WIFI,
                   is_connected() ? IC_RGB(0x30D158) : IC_RGB(IC_TINT_TEAL), title, sub);
    if (running()) {
        ic_ui_button(c, pb, (is_connected() || is_connecting()) ? "Disconnect" : "Scan", IC_SYM_NONE,
                     IC_BUTTON_DEFAULT, w.hover_btn == 0 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    }
    ic_ui_button(c, lb, w.show_log ? "Networks" : "Log", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 w.hover_btn == 1 ? IC_STATE_HOVER : IC_STATE_NORMAL);
}

static void draw_log(ic_canvas_t *c, ic_rect_t a) {
    const ic_palette_t *p = ic_palette();
    const ic_face_t *f = ic_font(IC_FONT_MONO_SMALL);
    ic_rect_t l = ic_rect_make(a.x, a.y + STATUS_H + IC_SP_4, a.w, a.h - STATUS_H - IC_SP_4);
    int lines = 0, total = 0, i, maxl, skip, y;
    int lh = f->line_h ? f->line_h : 14;

    if (!w.log[0]) {
        ic_ui_empty_state(c, l, IC_SYM_DOCUMENT, "No log yet",
                          "The Wi-Fi driver has not written anything");
        return;
    }
    ic_ui_group(c, l);
    for (i = 0; w.log[i]; i++) if (w.log[i] == '\n') total++;
    maxl = (l.h - IC_SP_3) / lh;
    skip = total > maxl ? total - maxl : 0;   /* show the newest lines */
    y = l.y + IC_SP_2;
    i = 0;
    while (w.log[i] && y + lh <= l.y + l.h) {
        char line[160];
        int s = i, n, fit;
        while (w.log[i] && w.log[i] != '\n') i++;
        n = i - s;
        if (w.log[i]) i++;
        if (lines++ < skip) continue;
        if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;
        ic_memcpy(line, w.log + s, (uint64_t)n);
        line[n] = 0;
        fit = ic_text_fit(f, line, l.w - 2 * IC_SP_3);
        if (fit < n) line[fit] = 0;
        ic_text_draw(c, f, l.x + IC_SP_3, y + f->ascent, line, p->label_secondary);
        y += lh;
    }
}

static void draw_networks(ic_canvas_t *c, ic_rect_t a) {
    ic_rect_t l = list_rect(a);
    int vis = visible_rows(a), count = w.nnets - w.scroll, i;

    ic_ui_section_header(c, a.x, a.y + STATUS_H + IC_SP_4, w.nnets ? "Networks" : "");
    if (!running()) return;
    if (w.notice[0]) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(a.x + 90, a.y + STATUS_H + IC_SP_4, a.w - 90, 16),
                        w.notice, ic_palette()->danger, IC_ALIGN_RIGHT);
    }
    if (w.nnets == 0) {
        ic_ui_empty_state(c, l, IC_SYM_WIFI, "No networks yet",
                          ic_streq(w.state, "scanning") ? "Scanning..." : "Press Scan to look again");
        return;
    }
    if (count > vis) count = vis;
    l.h = count * ROW_H;
    ic_ui_group(c, l);
    for (i = 0; i < count; i++) {
        const wifi_net_t *n = &w.nets[w.scroll + i];
        ic_rect_t r = net_row_rect(a, i);
        char sub[80], num[16];
        int connected_here = is_connected() && ic_streq(n->ssid, w.ssid);
        int secured = !ic_streq(n->sec, "open");
        int right = r.x + r.w - IC_SP_3;
        ic_ui_group_row(c, r, i, count, i == w.hover_row ? 1.0f : 0.0f);
        sub[0] = 0;
        ic_strlcat(sub, n->sec, sizeof(sub));
        ic_strlcat(sub, "  -  channel ", sizeof(sub));
        ic_uint_to_str((uint64_t)n->chan, num, sizeof(num));
        ic_strlcat(sub, num, sizeof(sub));
        ic_strlcat(sub, "  -  ", sizeof(sub));
        int_to_str(n->dbm, num, sizeof(num));
        ic_strlcat(sub, num, sizeof(sub));
        ic_strlcat(sub, " dBm", sizeof(sub));
        if (n->saved) ic_strlcat(sub, "  -  saved", sizeof(sub));
        if (n->saved) {
            ic_rect_t fr = forget_rect(r);
            ic_ui_icon_button(c, fr, IC_SYM_TRASH, w.hover_forget == i ? IC_STATE_HOVER : IC_STATE_NORMAL);
            right = fr.x - IC_SP_2;
        }
        if (connected_here) {
            ic_symbol_draw(c, IC_SYM_CHECK, (float)right - 8.0f, (float)r.y + r.h * 0.5f, 14.0f,
                           ic_palette()->accent);
            right -= 22;
        }
        if (secured) {
            ic_symbol_draw(c, IC_SYM_LOCK, (float)right - 8.0f, (float)r.y + r.h * 0.5f, 14.0f,
                           ic_palette()->label_secondary);
            right -= 22;
        }
        row_text_fit(c, ic_rect_make(r.x, r.y, right - r.x, r.h), IC_SYM_WIFI, signal_tint(n->pct),
                       n->ssid, sub);
    }
    if (w.nnets > vis) {
        ic_ui_scrollbar(c, ic_rect_make(l.x, l.y, l.w, vis * ROW_H), w.scroll * ROW_H, w.nnets * ROW_H, 1.0f);
    }
}

static void draw_sheet(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t r = sheet_rect(app);
    ic_textfield_t tf;
    char title[64], shown[65];
    int len = (int)ic_strlen(w.pass), i;

    ic_gfx_fill(c, 0, 0, app->width, app->height, IC_BLACK_A(64));
    ic_theme_shadow(c, r.x, r.y, r.w, r.h, IC_R_PANEL, IC_ELEV_MENU);
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_PANEL, ic_color_over(p->window, p->material_menu));
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_PANEL, 1.0f, p->frame);
    title[0] = 0;
    ic_strlcat(title, "Join \"", sizeof(title));
    ic_strlcat(title, w.sheet_ssid, sizeof(title));
    ic_strlcat(title, "\"", sizeof(title));
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(r.x + IC_SP_4, r.y + IC_SP_4, r.w - 2 * IC_SP_4, 20),
                    title, p->label, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + IC_SP_4, r.y + 38, r.w - 2 * IC_SP_4, 16),
                    "Enter the WPA2 password. Tab shows or hides it.", p->label_secondary, IC_ALIGN_LEFT);
    for (i = 0; i < len; i++) shown[i] = w.reveal ? w.pass[i] : '*';
    shown[len] = 0;
    tf.text = shown;
    tf.cursor = len;
    tf.sel_start = tf.sel_end = len;
    tf.focused = 1;
    tf.caret_on = ic_app_caret_visible(app);
    tf.placeholder = "Password";
    tf.leading = IC_SYM_LOCK;
    tf.scroll_px = 0;
    ic_ui_textfield_scroll(sheet_field(r), &tf);
    ic_ui_textfield(c, sheet_field(r), &tf);
    ic_ui_button(c, sheet_btn(r, 0), "Cancel", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 w.sheet_hover == 0 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_button(c, sheet_btn(r, 1), "Join", IC_SYM_NONE, IC_BUTTON_PRIMARY,
                 len >= 8 ? (w.sheet_hover == 1 ? IC_STATE_HOVER : IC_STATE_NORMAL) : IC_STATE_DISABLED);
}

void wifi_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t a) {
    draw_status(app, c, a);
    if (w.show_log) draw_log(c, a);
    else draw_networks(c, a);
    if (w.sheet) draw_sheet(app, c);
}

/* ---- input -------------------------------------------------------------- */

static void open_sheet(const char *ssid) {
    w.sheet = 1;
    ic_strcpy(w.sheet_ssid, ssid, sizeof(w.sheet_ssid));
    ic_memzero(w.pass, sizeof(w.pass));
    w.reveal = 0;
    w.sheet_hover = -1;
}

static void close_sheet(int join) {
    if (join && ic_strlen(w.pass) >= 8) connect_to(w.sheet_ssid, w.pass);
    ic_memzero(w.pass, sizeof(w.pass));
    w.sheet = 0;
}

static void click_network(int i) {
    const wifi_net_t *n = &w.nets[i];
    w.notice[0] = 0;
    if (is_connected() && ic_streq(n->ssid, w.ssid)) return;
    if (ic_streq(n->sec, "open") || n->saved) {
        connect_to(n->ssid, "");
        return;
    }
    if (!ic_streq(n->sec, "WPA2")) {
        ic_strcpy(w.notice, ic_streq(n->sec, "WPA/WPA2") ?
                  "Mixed WPA/WPA2 (TKIP) is not supported; use WPA2-AES" :
                  "Only WPA2-Personal and open networks are supported", sizeof(w.notice));
        return;
    }
    open_sheet(n->ssid);
}

static int sheet_event(ic_app_t *app, const ic_event_t *ev) {
    ic_rect_t r = sheet_rect(app);
    int len = (int)ic_strlen(w.pass);
    switch (ev->type) {
    case IC_EV_KEY:
        if (ev->key == IC_KEY_ESCAPE) close_sheet(0);
        else if (ev->key == IC_KEY_ENTER) close_sheet(1);
        else if (ev->key == IC_KEY_TAB) w.reveal = !w.reveal;
        else if (ev->key == IC_KEY_BACKSPACE) { if (len > 0) w.pass[len - 1] = 0; }
        else if (ev->key >= 32 && ev->key < 127 && len < 63) { w.pass[len] = (char)ev->key; w.pass[len + 1] = 0; }
        ic_app_caret_reset(app);
        return 1;
    case IC_EV_MOUSE_MOVE:
        w.sheet_hover = ic_ui_hit(sheet_btn(r, 0), ev->x, ev->y) ? 0 :
                        ic_ui_hit(sheet_btn(r, 1), ev->x, ev->y) ? 1 : -1;
        return 1;
    case IC_EV_MOUSE_DOWN:
        if (ic_ui_hit(sheet_btn(r, 0), ev->x, ev->y)) close_sheet(0);
        else if (ic_ui_hit(sheet_btn(r, 1), ev->x, ev->y)) close_sheet(1);
        return 1;
    default:
        return 1;
    }
}

int wifi_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t a) {
    int vis = visible_rows(a), count, i;

    if (w.sheet) return sheet_event(app, ev);
    count = w.nnets - w.scroll;
    if (count > vis) count = vis;
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
    case IC_EV_MOUSE_LEAVE:
        w.hover_row = w.hover_forget = w.hover_btn = -1;
        if (ic_ui_hit(primary_btn_rect(a), ev->x, ev->y)) w.hover_btn = 0;
        else if (ic_ui_hit(log_btn_rect(a), ev->x, ev->y)) w.hover_btn = 1;
        if (!w.show_log) {
            for (i = 0; i < count; i++) {
                ic_rect_t r = net_row_rect(a, i);
                if (!ic_ui_hit(r, ev->x, ev->y)) continue;
                w.hover_row = i;
                if (w.nets[w.scroll + i].saved && ic_ui_hit(forget_rect(r), ev->x, ev->y)) w.hover_forget = i;
            }
        }
        return 0;
    case IC_EV_MOUSE_DOWN:
        if (running() && ic_ui_hit(primary_btn_rect(a), ev->x, ev->y)) {
            if (is_connected() || is_connecting()) { (void)wifi_cmd("disconnect"); poll(1); }
            else do_scan();
            return 1;
        }
        if (ic_ui_hit(log_btn_rect(a), ev->x, ev->y)) {
            w.show_log = !w.show_log;
            poll(1);
            return 1;
        }
        if (!w.show_log && running()) {
            for (i = 0; i < count; i++) {
                ic_rect_t r = net_row_rect(a, i);
                if (!ic_ui_hit(r, ev->x, ev->y)) continue;
                if (w.nets[w.scroll + i].saved && ic_ui_hit(forget_rect(r), ev->x, ev->y))
                    forget(w.nets[w.scroll + i].ssid);
                else
                    click_network(w.scroll + i);
                return 1;
            }
        }
        return 0;
    case IC_EV_SCROLL:
        if (!w.show_log && w.nnets > vis) {
            w.scroll += ev->wheel > 0 ? 1 : -1;
            if (w.scroll > w.nnets - vis) w.scroll = w.nnets - vis;
            if (w.scroll < 0) w.scroll = 0;
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}
