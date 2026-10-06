/*
 * Taskbar Wi-Fi flyout: status, nearby networks, joining (with an inline
 * password field for WPA2) and a way into Settings > Wi-Fi.  Talks to the
 * driver through /dev/wifi like Settings does.
 */

#include "wm_wifi.h"
#include "wm_shell.h"

#define WIFI_DEV     "/dev/wifi"
#define NET_MAX      24
#define ROWS_MAX     7
#define REPORT_CAP   8192

#define FLY_W        340
#define FLY_PAD      IC_SP_4
#define HEAD_H       58
#define ROW_H        40
#define PASS_H       (IC_H_CONTROL + IC_SP_3)
#define NOTE_H       20
#define FOOT_H       (IC_H_CONTROL + IC_SP_4)
#define FLY_GAP      8

typedef struct {
    char ssid[33];
    char sec[12];
    int  pct;
    int  saved;
} net_t;

static struct {
    char     state[24];
    char     ssid[40];
    char     ip[20];
    char     error[100];
    net_t    nets[NET_MAX];
    int      nnets;
    uint32_t last_poll;
    int      polled;
    /* flyout */
    int      hover;              /* row, or one of the HOVER_* below */
    char     join[33];           /* network waiting for a password */
    char     pass[65];
    int      reveal;
    char     notice[100];
} w;

#define HOVER_NONE    (-1)
#define HOVER_JOIN    (-2)
#define HOVER_PRIMARY (-3)
#define HOVER_SETTINGS (-4)

/* ---- /dev/wifi ---------------------------------------------------------- */

static int wifi_cmd(const char *cmd) {
    return icda_write_file(WIFI_DEV, cmd, ic_strlen(cmd)) == (uint64_t)-1 ? -1 : 0;
}

static void copy_value(char *dst, int cap, const char *src, int len) {
    int n = len < cap - 1 ? len : cap - 1;
    if (n < 0) n = 0;
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

/* "key: value" lines, then "--- networks" and TAB-separated rows:
 * pct, dBm, channel, security, saved flag, SSID. */
static void parse_report(const char *r) {
    int i = 0, in_nets = 0;
    w.state[0] = w.ssid[0] = w.ip[0] = w.error[0] = 0;
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
            net_t *n;
            int p = s, f;
            if (w.nnets >= NET_MAX) continue;
            n = &w.nets[w.nnets];
            n->pct = parse_int(r, &p); if (r[p] == '\t') p++;
            (void)parse_int(r, &p); if (r[p] == '\t') p++;
            (void)parse_int(r, &p); if (r[p] == '\t') p++;
            for (f = p; f < e && r[f] != '\t'; f++) {}
            copy_value(n->sec, sizeof(n->sec), r + p, f - p);
            p = f + 1;
            for (f = p; f < e && r[f] != '\t'; f++) {}
            n->saved = (f - p == 5 && r[p] == 's');
            p = f + 1;
            if (p >= e) continue;
            copy_value(n->ssid, sizeof(n->ssid), r + p, e - p);
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
            FIELD("error", w.error);
#undef FIELD
        }
    }
}

static int is_connected(void) {
    return ic_streq(w.state, "connected") || ic_streq(w.state, "no-ip") || ic_streq(w.state, "dhcp");
}

static int is_connecting(void) {
    return ic_streq(w.state, "searching") || ic_streq(w.state, "authenticating") ||
           ic_streq(w.state, "associating") || ic_streq(w.state, "handshake");
}

static int running(void) {
    return w.state[0] && !ic_streq(w.state, "no-adapter") && !ic_streq(w.state, "starting") &&
           !ic_streq(w.state, "error") && !ic_streq(w.state, "radio-off");
}

int wm_wifi_bar_state(void) {
    if (!w.state[0] || ic_streq(w.state, "no-adapter")) return WM_WIFI_NONE;
    if (!running()) return WM_WIFI_OFF;
    if (ic_streq(w.state, "connected")) return WM_WIFI_ONLINE;
    if (is_connected() || is_connecting()) return WM_WIFI_BUSY;
    return WM_WIFI_IDLE;
}

/* cheap fingerprint of what the flyout shows */
static uint32_t view_hash(void) {
    uint32_t h = 2166136261u;
    const char *parts[4] = { w.state, w.ssid, w.ip, w.error };
    for (int k = 0; k < 4; k++)
        for (const char *s = parts[k]; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (int i = 0; i < w.nnets; i++) {
        for (const char *s = w.nets[i].ssid; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
        h = (h ^ (uint32_t)(w.nets[i].pct / 10)) * 16777619u;
        h = (h ^ (uint32_t)w.nets[i].saved) * 16777619u;
    }
    return h;
}

int wm_wifi_poll(uint32_t min_ms) {
    static char report[REPORT_CAP];
    uint32_t now = ic_time_ms();
    int bar_before = wm_wifi_bar_state();
    uint32_t view_before = view_hash();
    int changed = 0;

    if (w.polled && now - w.last_poll < min_ms) return 0;
    w.polled = 1;
    w.last_poll = now;
    if (wifi_cmd("all") != 0) {
        w.state[0] = 0;
        w.nnets = 0;
    } else {
        long n = (long)icda_read_file(WIFI_DEV, report, sizeof(report) - 1);
        report[n > 0 ? n : 0] = 0;
        parse_report(report);
    }
    if (wm_wifi_bar_state() != bar_before) changed |= 1;
    if (view_hash() != view_before) changed |= 2;
    return changed;
}

/* ---- layout ------------------------------------------------------------- */

static int visible_rows(void) {
    int n = w.nnets < ROWS_MAX ? w.nnets : ROWS_MAX;
    return n > 0 ? n : 1;   /* one row for "no networks" */
}

static int fly_height(int rows, int with_pass, int with_note) {
    return HEAD_H + rows * ROW_H + (with_pass ? PASS_H : 0) + (with_note ? NOTE_H : 0) + FOOT_H;
}

static ic_rect_t fly_at(int sw, int sh, int h) {
    return ic_rect_make(sw - FLY_W - FLY_GAP, sh - WM_BAR_H - FLY_GAP - h, FLY_W, h);
}

ic_rect_t wm_wifi_rect(int sw, int sh) {
    return fly_at(sw, sh, fly_height(visible_rows(), w.join[0] != 0, w.notice[0] != 0));
}

ic_rect_t wm_wifi_reach(int sw, int sh) {
    return fly_at(sw, sh, fly_height(ROWS_MAX, 1, 1));
}

static ic_rect_t row_rect(ic_rect_t f, int i) {
    return ic_rect_make(f.x + IC_SP_2, f.y + HEAD_H + i * ROW_H, f.w - 2 * IC_SP_2, ROW_H);
}

static ic_rect_t pass_field(ic_rect_t f) {
    int y = f.y + HEAD_H + visible_rows() * ROW_H + IC_SP_1;
    return ic_rect_make(f.x + FLY_PAD, y, f.w - 2 * FLY_PAD - 72 - IC_SP_2, IC_H_CONTROL);
}

static ic_rect_t join_btn(ic_rect_t f) {
    ic_rect_t pf = pass_field(f);
    return ic_rect_make(pf.x + pf.w + IC_SP_2, pf.y, 72, IC_H_CONTROL);
}

static ic_rect_t foot_btn(ic_rect_t f, int which) {
    int bw = (f.w - 2 * FLY_PAD - IC_SP_2) / 2;
    int y = f.y + f.h - FOOT_H + (FOOT_H - IC_H_CONTROL) / 2;
    return ic_rect_make(f.x + FLY_PAD + which * (bw + IC_SP_2), y, bw, IC_H_CONTROL);
}

/* ---- drawing ------------------------------------------------------------ */

static ic_color_t signal_tint(int pct) {
    if (pct >= 60) return IC_RGB(0x30D158);
    if (pct >= 35) return IC_RGB(IC_TINT_ORANGE);
    return IC_RGB(0xFF453A);
}

static void status_text(char *title, int tcap, char *sub, int scap) {
    title[0] = sub[0] = 0;
    if (!w.state[0] || ic_streq(w.state, "no-adapter")) {
        ic_strlcat(title, "No Wi-Fi adapter", tcap);
        ic_strlcat(sub, "Supported: Intel Wireless-AC 8260", scap);
    } else if (ic_streq(w.state, "starting")) {
        ic_strlcat(title, "Starting Wi-Fi...", tcap);
        ic_strlcat(sub, "Loading the firmware", scap);
    } else if (!running()) {
        ic_strlcat(title, ic_streq(w.state, "radio-off") ? "Wi-Fi is off" : "Wi-Fi failed to start", tcap);
        ic_strlcat(sub, w.error[0] ? w.error : "Open Settings for the log", scap);
    } else if (is_connected()) {
        ic_strlcat(title, w.ssid, tcap);
        ic_strlcat(sub, ic_streq(w.state, "dhcp") ? "Connected, getting an address..." :
                        ic_streq(w.state, "no-ip") ? "Connected, no IP address" : "Connected", scap);
        if (ic_streq(w.state, "connected") && w.ip[0]) {
            ic_strlcat(sub, "  -  ", scap);
            ic_strlcat(sub, w.ip, scap);
        }
    } else if (is_connecting()) {
        ic_strlcat(title, w.ssid, tcap);
        ic_strlcat(sub, ic_streq(w.state, "handshake") ? "Checking the password..." : "Connecting...", scap);
    } else {
        ic_strlcat(title, "Not connected", tcap);
        ic_strlcat(sub, w.error[0] ? w.error :
                        ic_streq(w.state, "scanning") ? "Scanning..." : "Choose a network", scap);
    }
}

void wm_wifi_draw(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t f = wm_wifi_rect(sw, sh);
    char title[80], sub[140];
    int rows = w.nnets < ROWS_MAX ? w.nnets : ROWS_MAX;

    ic_ui_panel(c, f, IC_R_PANEL, IC_ELEV_MENU, scratch, scratch_len);

    status_text(title, sizeof(title), sub, sizeof(sub));
    ic_symbol_draw(c, IC_SYM_WIFI, (float)(f.x + FLY_PAD + 10), (float)(f.y + HEAD_H / 2), 20.0f,
                   wm_wifi_bar_state() == WM_WIFI_ONLINE ? p->accent : p->label_secondary);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH),
                    ic_rect_make(f.x + FLY_PAD + 30, f.y + 12, f.w - 2 * FLY_PAD - 30, 18),
                    title, p->label, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                    ic_rect_make(f.x + FLY_PAD + 30, f.y + 32, f.w - 2 * FLY_PAD - 30, 16),
                    sub, p->label_secondary, IC_ALIGN_LEFT);
    ic_gfx_hline(c, f.x + FLY_PAD, f.y + HEAD_H - 4, f.w - 2 * FLY_PAD, p->separator);

    if (!running() || rows == 0) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), row_rect(f, 0),
                        running() ? "No networks found yet" : "", p->label_tertiary, IC_ALIGN_CENTER);
    }
    for (int i = 0; running() && i < rows; i++) {
        const net_t *n = &w.nets[i];
        ic_rect_t r = row_rect(f, i);
        int here = is_connected() && ic_streq(n->ssid, w.ssid);
        int pending = w.join[0] && ic_streq(n->ssid, w.join);
        int right = r.x + r.w - IC_SP_2;
        if (w.hover == i || pending)
            ic_gfx_rrect(c, r.x, r.y + 2, r.w, r.h - 4, IC_R_CONTROL, p->fill_hover);
        ic_symbol_draw(c, IC_SYM_WIFI, (float)(r.x + 18), (float)(r.y + r.h / 2), 16.0f, signal_tint(n->pct));
        if (here) {
            ic_symbol_draw(c, IC_SYM_CHECK, (float)right - 8.0f, (float)(r.y + r.h / 2), 14.0f, p->accent);
            right -= 22;
        }
        if (!ic_streq(n->sec, "open")) {
            ic_symbol_draw(c, IC_SYM_LOCK, (float)right - 8.0f, (float)(r.y + r.h / 2), 13.0f,
                           p->label_secondary);
            right -= 22;
        }
        ic_text_draw_in(c, ic_font(here ? IC_FONT_BODY_EMPH : IC_FONT_BODY),
                        ic_rect_make(r.x + 36, r.y, right - r.x - 40, r.h / 2 + 6),
                        n->ssid, p->label, IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_CAPTION),
                        ic_rect_make(r.x + 36, r.y + r.h / 2 + 2, right - r.x - 40, r.h / 2 - 4),
                        here ? "Connected" : n->saved ? "Saved" :
                        ic_streq(n->sec, "open") ? "Open" : n->sec,
                        p->label_secondary, IC_ALIGN_LEFT);
    }

    if (w.join[0]) {
        ic_textfield_t tf;
        char shown[65];
        int len = (int)ic_strlen(w.pass);
        for (int i = 0; i < len; i++) shown[i] = w.reveal ? w.pass[i] : '*';
        shown[len] = 0;
        tf.text = shown;
        tf.cursor = len;
        tf.sel_start = tf.sel_end = len;
        tf.focused = 1;
        tf.caret_on = 1;
        tf.placeholder = "Password";
        tf.leading = IC_SYM_LOCK;
        tf.scroll_px = 0;
        ic_ui_textfield_scroll(pass_field(f), &tf);
        ic_ui_textfield(c, pass_field(f), &tf);
        ic_ui_button(c, join_btn(f), "Join", IC_SYM_NONE, IC_BUTTON_PRIMARY,
                     len < 8 ? IC_STATE_DISABLED : w.hover == HOVER_JOIN ? IC_STATE_HOVER : IC_STATE_NORMAL);
    }
    if (w.notice[0]) {
        int y = f.y + f.h - FOOT_H - NOTE_H;
        ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(f.x + FLY_PAD, y, f.w - 2 * FLY_PAD, NOTE_H),
                        w.notice, p->danger, IC_ALIGN_LEFT);
    }

    ic_ui_button(c, foot_btn(f, 0), (is_connected() || is_connecting()) ? "Disconnect" : "Scan",
                 IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 !running() ? IC_STATE_DISABLED : w.hover == HOVER_PRIMARY ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_button(c, foot_btn(f, 1), "Wi-Fi Settings", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 w.hover == HOVER_SETTINGS ? IC_STATE_HOVER : IC_STATE_NORMAL);
}

/* ---- input -------------------------------------------------------------- */

void wm_wifi_open(void) {
    w.hover = HOVER_NONE;
    w.join[0] = 0;
    ic_memzero(w.pass, sizeof(w.pass));
    w.reveal = 0;
    w.notice[0] = 0;
    if (running() && !is_connecting()) (void)wifi_cmd("scan");
    (void)wm_wifi_poll(0);
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
    w.join[0] = 0;
    ic_memzero(w.pass, sizeof(w.pass));
    (void)wm_wifi_poll(0);
}

static void click_network(const net_t *n) {
    w.notice[0] = 0;
    if (is_connected() && ic_streq(n->ssid, w.ssid)) return;
    if (ic_streq(n->sec, "open") || n->saved) {
        connect_to(n->ssid, "");
        return;
    }
    if (!ic_streq(n->sec, "WPA2")) {
        ic_strcpy(w.notice, ic_streq(n->sec, "WPA/WPA2") ? "Mixed WPA/WPA2 (TKIP) is not supported" :
                            "Only WPA2-Personal and open networks are supported", sizeof(w.notice));
        return;
    }
    if (ic_streq(w.join, n->ssid)) return;
    ic_strcpy(w.join, n->ssid, sizeof(w.join));
    ic_memzero(w.pass, sizeof(w.pass));
    w.reveal = 0;
}

static int hit_at(int sw, int sh, int mx, int my) {
    ic_rect_t f = wm_wifi_rect(sw, sh);
    int rows = w.nnets < ROWS_MAX ? w.nnets : ROWS_MAX;
    if (running())
        for (int i = 0; i < rows; i++)
            if (ic_ui_hit(row_rect(f, i), mx, my)) return i;
    if (w.join[0] && ic_ui_hit(join_btn(f), mx, my)) return HOVER_JOIN;
    if (ic_ui_hit(foot_btn(f, 0), mx, my)) return HOVER_PRIMARY;
    if (ic_ui_hit(foot_btn(f, 1), mx, my)) return HOVER_SETTINGS;
    return HOVER_NONE;
}

int wm_wifi_hover(int sw, int sh, int mx, int my) {
    int h = hit_at(sw, sh, mx, my);
    if (h == w.hover) return WM_WIFI_KEEP;
    w.hover = h;
    return WM_WIFI_REDRAW;
}

int wm_wifi_click(int sw, int sh, int mx, int my) {
    int h = hit_at(sw, sh, mx, my);
    if (h >= 0) {
        click_network(&w.nets[h]);
        return WM_WIFI_REDRAW;
    }
    if (h == HOVER_JOIN && ic_strlen(w.pass) >= 8) {
        char ssid[33];
        ic_strcpy(ssid, w.join, sizeof(ssid));
        connect_to(ssid, w.pass);
        return WM_WIFI_REDRAW;
    }
    if (h == HOVER_PRIMARY && running()) {
        if (is_connected() || is_connecting()) (void)wifi_cmd("disconnect");
        else (void)wifi_cmd("scan");
        (void)wm_wifi_poll(0);
        return WM_WIFI_REDRAW;
    }
    if (h == HOVER_SETTINGS) {
        icda_spawn_args("/apps/settings.app", "wifi");
        return WM_WIFI_CLOSE;
    }
    return WM_WIFI_KEEP;
}

int wm_wifi_key(int key) {
    int len = (int)ic_strlen(w.pass);
    if (!w.join[0]) return WM_WIFI_KEEP;
    if (key == 13) {
        if (len >= 8) {
            char ssid[33];
            ic_strcpy(ssid, w.join, sizeof(ssid));
            connect_to(ssid, w.pass);
        }
    } else if (key == 9) {
        w.reveal = !w.reveal;
    } else if (key == 8 || key == 127) {
        if (len > 0) w.pass[len - 1] = 0;
    } else if (key >= 32 && key < 127 && len < 63) {
        w.pass[len] = (char)key;
        w.pass[len + 1] = 0;
    } else {
        return WM_WIFI_KEEP;
    }
    return WM_WIFI_REDRAW;
}
