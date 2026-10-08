/* Activity: what the machine is doing.  Cards on top show CPU use (with
 * the last minute as a graph), memory in use and the process count; the
 * table below lists every process with its share of the processors and
 * its memory.  Click a column title to sort by it. */
#include "libicda.h"

#define WIN_W 780
#define WIN_H 560

#define TM_MAX_PROCS     96
#define TM_BUF_CAP       12288
#define TM_STATUS_CAP    160
#define TM_STORAGE_CAP   512
#define TM_SAMPLE_TICKS  100          /* one second */
#define TM_HISTORY       60
#define TM_FOOTER_H      26
#define TM_CARDS_H       96

/* table columns, right-aligned numbers after the name */
#define COL_PID_W    64
#define COL_STATE_W  100
#define COL_CPU_W    96
#define COL_MEM_W    100

enum { TM_NONE = 0, TM_KILL, TM_SUSPEND };
enum { SORT_NAME = 0, SORT_PID, SORT_STATE, SORT_CPU, SORT_MEM };

typedef struct {
    uint64_t pid;
    char     name[64];
    char     state[16];
    uint64_t cpu_ticks;
    uint64_t mem_bytes;
    uint32_t cpu_tenths;          /* share of all processors, 0.1 % units */
    int      suspended;
} tm_proc_t;

typedef struct {
    uint64_t pid;
    uint64_t cpu_ticks;
} tm_prev_t;

static struct {
    tm_proc_t procs[TM_MAX_PROCS];
    int       count;
    int       selected;
    int       sort;

    tm_prev_t prev[TM_MAX_PROCS];   /* last sample's CPU time, by pid */
    int       prev_count;
    uint64_t  prev_uptime, prev_idle;
    int       have_prev;

    icda_sys_stats_t sys;
    uint32_t  cpu_tenths;            /* whole machine */
    uint8_t   history[TM_HISTORY];   /* CPU % per second, oldest first */
    int       history_n;

    int rows;
    int scroll;
    int first_row;
    int last_row;

    int hover_row;
    int hover_refresh;
    int hover_suspend;
    int hover_kill;
    int hover_col;
    int list_focused;

    int alert_action;
    int alert_hover;

    char     storage[TM_STORAGE_CAP];
    char     status[TM_STATUS_CAP];
    uint64_t last_sample;
} tm;

/* ---- text ------------------------------------------------------------------ */

static void tm_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void tm_u64(uint64_t v, char *dst, uint64_t cap) {
    char tmp[24];
    int n = 0;
    int i = 0;
    if (cap == 0) return;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 24);
    while (n > 0 && (uint64_t)i + 1 < cap) dst[i++] = tmp[--n];
    dst[i] = 0;
}

static void tm_cat_u64(char *dst, uint64_t v, uint64_t cap) {
    char d[24];
    tm_u64(v, d, sizeof(d));
    ic_strlcat(dst, d, cap);
}

/* v in tenths -> "12.3" */
static void tm_tenths(char *dst, uint64_t tenths, uint64_t cap) {
    char d[4] = { '.', 0, 0, 0 };
    dst[0] = 0;
    tm_cat_u64(dst, tenths / 10, cap);
    d[1] = (char)('0' + tenths % 10);
    ic_strlcat(dst, d, cap);
}

/* bytes -> "512 KB", "4.2 MB", "312 MB", "1.6 GB" */
static void tm_bytes(char *dst, uint64_t bytes, uint64_t cap) {
    const uint64_t mb = 1024ULL * 1024ULL, gb = mb * 1024ULL;
    dst[0] = 0;
    if (bytes >= gb) {
        tm_tenths(dst, (bytes * 10 + gb / 2) / gb, cap);
        ic_strlcat(dst, " GB", cap);
    } else if (bytes >= 10 * mb) {
        tm_cat_u64(dst, (bytes + mb / 2) / mb, cap);
        ic_strlcat(dst, " MB", cap);
    } else if (bytes >= mb) {
        tm_tenths(dst, (bytes * 10 + mb / 2) / mb, cap);
        ic_strlcat(dst, " MB", cap);
    } else if (bytes) {
        tm_cat_u64(dst, (bytes + 1023) / 1024, cap);
        ic_strlcat(dst, " KB", cap);
    } else {
        ic_strlcat(dst, "-", cap);
    }
}

static void tm_status(const char *text) {
    tm_copy(tm.status, text, TM_STATUS_CAP);
}

static void tm_status_name(const char *prefix, const char *name) {
    tm.status[0] = 0;
    ic_strlcat(tm.status, prefix, TM_STATUS_CAP);
    ic_strlcat(tm.status, name, TM_STATUS_CAP);
}

static int tm_atoi(const char *s) {
    uint64_t v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
    return (int)v;
}

static const char *state_label(const tm_proc_t *p) {
    if (p->suspended) return "Suspended";
    if (ic_streq(p->state, "running")) return "Running";
    if (ic_streq(p->state, "ready")) return "Ready";
    if (ic_streq(p->state, "blocked")) return "Waiting";
    if (ic_streq(p->state, "new")) return "Starting";
    if (ic_streq(p->state, "exited") || ic_streq(p->state, "reaped")) return "Exited";
    return p->state;
}

static int has_selection(void) {
    return tm.selected >= 0 && tm.selected < tm.count;
}

/* ---- layout ------------------------------------------------------------------ */

static ic_rect_t toolbar_rect(ic_app_t *app) {
    return ic_rect_make(0, 0, app->width, IC_H_TOOLBAR);
}

static ic_rect_t card_rect(ic_app_t *app, int i) {
    int x = IC_SP_4, y = IC_H_TOOLBAR + IC_SP_3, w = app->width - 2 * IC_SP_4, gap = IC_SP_3;
    int w0 = (w - 2 * gap) * 2 / 5, w2 = w - 2 * gap - 2 * w0;
    if (i == 0) return ic_rect_make(x, y, w0, TM_CARDS_H);
    if (i == 1) return ic_rect_make(x + w0 + gap, y, w0, TM_CARDS_H);
    return ic_rect_make(x + 2 * (w0 + gap), y, w2, TM_CARDS_H);
}

static ic_rect_t table_rect(ic_app_t *app) {
    int y = IC_H_TOOLBAR + IC_SP_3 + TM_CARDS_H + IC_SP_4 + IC_H_ROW;
    return ic_rect_make(IC_SP_4, y, app->width - 2 * IC_SP_4, app->height - y - TM_FOOTER_H - IC_SP_2);
}

static ic_rect_t header_rect(ic_app_t *app) {
    ic_rect_t t = table_rect(app);
    return ic_rect_make(t.x, t.y - IC_H_ROW - 2, t.w, IC_H_ROW);
}

static ic_rect_t footer_rect(ic_app_t *app) {
    return ic_rect_make(0, app->height - TM_FOOTER_H, app->width, TM_FOOTER_H);
}

static ic_rect_t row_rect(ic_app_t *app, int row) {
    ic_rect_t t = table_rect(app);
    return ic_rect_make(t.x, t.y + row * IC_H_ROW, t.w, IC_H_ROW);
}

/* column widths: the name takes what the others leave */
static void column_widths(ic_app_t *app, int w[5]) {
    ic_rect_t t = table_rect(app);
    w[1] = COL_PID_W;
    w[2] = COL_STATE_W;
    w[3] = COL_CPU_W;
    w[4] = COL_MEM_W;
    w[0] = t.w - w[1] - w[2] - w[3] - w[4];
    if (w[0] < 120) w[0] = 120;
}

static int column_at(ic_app_t *app, int x, int y) {
    ic_rect_t h = header_rect(app);
    int w[5], cx = h.x;
    if (!ic_ui_hit(h, x, y)) return -1;
    column_widths(app, w);
    for (int i = 0; i < 5; i++) {
        if (x >= cx && x < cx + w[i]) return i;
        cx += w[i];
    }
    return -1;
}

static void layout(ic_app_t *app) {
    ic_rect_t t = table_rect(app);
    int rows = t.h / IC_H_ROW;
    if (rows < 1) rows = 1;
    tm.rows = rows;
    if (tm.scroll > tm.count - rows) tm.scroll = tm.count - rows;
    if (tm.scroll < 0) tm.scroll = 0;
    tm.first_row = tm.scroll;
    tm.last_row = tm.scroll + rows;
    if (tm.last_row > tm.count) tm.last_row = tm.count;
}

static ic_rect_t refresh_rect(ic_app_t *app) {
    ic_rect_t b = toolbar_rect(app);
    return ic_rect_make(b.x + IC_SP_4, (b.h - IC_H_CONTROL_SM) / 2, 30, IC_H_CONTROL_SM);
}

static int suspend_label_is_resume(void) {
    return has_selection() && tm.procs[tm.selected].suspended;
}

static ic_rect_t suspend_rect(ic_app_t *app) {
    ic_rect_t b = toolbar_rect(app);
    ic_rect_t r = refresh_rect(app);
    int w = ic_ui_button_width(suspend_label_is_resume() ? "Resume" : "Suspend", IC_SYM_NONE);
    return ic_rect_make(r.x + r.w + IC_SP_2, (b.h - IC_H_CONTROL_SM) / 2, w, IC_H_CONTROL_SM);
}

static ic_rect_t kill_rect(ic_app_t *app) {
    ic_rect_t r = suspend_rect(app);
    int w = ic_ui_button_width("Quit Process", IC_SYM_NONE);
    return ic_rect_make(r.x + r.w + IC_SP_2, r.y, w, IC_H_CONTROL_SM);
}

static ic_rect_t alert_rect(ic_app_t *app) {
    int w = 380, h = ic_ui_alert_height(IC_SYM_INFO, "");
    return ic_rect_make((app->width - w) / 2, (app->height - h) / 2, w, h);
}

static ic_rect_t alert_button_rect(ic_app_t *app, int index) {
    return ic_ui_alert_button_rect(alert_rect(app), index, 2);
}

/* ---- sampling ------------------------------------------------------------------ */

static uint64_t prev_ticks_of(uint64_t pid, int *found) {
    for (int i = 0; i < tm.prev_count; i++)
        if (tm.prev[i].pid == pid) {
            *found = 1;
            return tm.prev[i].cpu_ticks;
        }
    *found = 0;
    return 0;
}

static int compare(const tm_proc_t *a, const tm_proc_t *b) {
    switch (tm.sort) {
    case SORT_NAME: {
        int i = 0;
        while (a->name[i] && a->name[i] == b->name[i]) i++;
        if (a->name[i] != b->name[i]) return (unsigned char)a->name[i] < (unsigned char)b->name[i] ? -1 : 1;
        break;
    }
    case SORT_STATE: {
        const char *x = state_label(a), *y = state_label(b);
        int i = 0;
        while (x[i] && x[i] == y[i]) i++;
        if (x[i] != y[i]) return (unsigned char)x[i] < (unsigned char)y[i] ? -1 : 1;
        break;
    }
    case SORT_CPU:
        if (a->cpu_tenths != b->cpu_tenths) return a->cpu_tenths > b->cpu_tenths ? -1 : 1;
        if (a->mem_bytes != b->mem_bytes) return a->mem_bytes > b->mem_bytes ? -1 : 1;
        break;
    case SORT_MEM:
        if (a->mem_bytes != b->mem_bytes) return a->mem_bytes > b->mem_bytes ? -1 : 1;
        if (a->cpu_tenths != b->cpu_tenths) return a->cpu_tenths > b->cpu_tenths ? -1 : 1;
        break;
    }
    return a->pid < b->pid ? -1 : a->pid > b->pid ? 1 : 0;
}

static void sort_procs(void) {
    uint64_t sel = has_selection() ? tm.procs[tm.selected].pid : 0;
    for (int i = 1; i < tm.count; i++) {
        tm_proc_t v = tm.procs[i];
        int j = i - 1;
        while (j >= 0 && compare(&tm.procs[j], &v) > 0) {
            tm.procs[j + 1] = tm.procs[j];
            j--;
        }
        tm.procs[j + 1] = v;
    }
    tm.selected = -1;
    for (int i = 0; sel && i < tm.count; i++)
        if (tm.procs[i].pid == sel) tm.selected = i;
}

static void parse_procs(const char *buf, uint64_t len, uint64_t wall) {
    uint64_t pos = 0, cpus = tm.sys.cpus ? tm.sys.cpus : 1;
    tm.count = 0;
    while (pos < len && tm.count < TM_MAX_PROCS) {
        char line[256];
        uint64_t li = 0;
        char *tok[8];
        char *p;
        int fi = 0, pid, seen;
        uint64_t before;
        tm_proc_t *out;

        while (pos < len && buf[pos] != '\n' && li + 1 < sizeof(line)) line[li++] = buf[pos++];
        while (pos < len && buf[pos] != '\n') pos++;
        if (pos < len) pos++;
        line[li] = 0;
        if (li == 0) continue;
        if (line[0] == 'p' && line[1] == 'i' && line[2] == 'd') continue;

        p = line;
        tok[fi++] = p;
        while (*p && fi < 8) {
            if (*p == ' ') { *p = 0; p++; tok[fi++] = p; }
            else p++;
        }
        if (fi < 6) continue;
        pid = tm_atoi(tok[0]);
        if (pid <= 0) continue;
        if (ic_streq(tok[5], "exited") || ic_streq(tok[5], "reaped")) continue;

        out = &tm.procs[tm.count];
        out->pid = (uint64_t)pid;
        tm_copy(out->name, "?", sizeof(out->name));
        tm_copy(out->state, tok[5], sizeof(out->state));
        out->cpu_ticks = 0;
        out->mem_bytes = 0;
        out->cpu_tenths = 0;
        out->suspended = ic_streq(tok[5], "stopped");
        {
            icda_proc_stats_t st;
            if (icda_proc_stats((uint64_t)pid, &st) == 0) {
                tm_copy(out->name, st.name, sizeof(out->name));
                out->cpu_ticks = st.cpu_ticks;
                out->mem_bytes = st.mem_bytes;
            }
        }
        if (!out->name[0] || ic_streq(out->name, "?")) {
            tm_copy(out->name, ic_streq(tok[4], "kernel") ? "kernel_task" : "?", sizeof(out->name));
        }
        if (ic_streq(out->name, "idle") && ic_streq(tok[4], "kernel")) continue;   /* the processors' idle time */
        /* share of all processors since the last sample */
        before = prev_ticks_of(out->pid, &seen);
        if (seen && wall && out->cpu_ticks >= before) {
            uint64_t t = (out->cpu_ticks - before) * 1000 / (wall * cpus);
            out->cpu_tenths = (uint32_t)(t > 1000 ? 1000 : t);
        }
        tm.count++;
    }
    tm.prev_count = 0;
    for (int i = 0; i < tm.count; i++) {
        tm.prev[tm.prev_count].pid = tm.procs[i].pid;
        tm.prev[tm.prev_count].cpu_ticks = tm.procs[i].cpu_ticks;
        tm.prev_count++;
    }
}

static void summarize_storage(void) {
    int section = 0, counts[3] = { 0, 0, 0 };
    char *s = tm.storage;

    while (*s) {
        char *line = s;
        while (*s && *s != '\n') s++;
        if (*s) *s++ = 0;
        if (line[0] != ' ') {
            if (ic_streq(line, "devices:")) section = 0;
            else if (ic_streq(line, "partitions:")) section = 1;
            else if (ic_streq(line, "mounts:")) section = 2;
        } else if (!ic_streq(line, "  (none)")) {
            counts[section]++;
        }
    }
    tm.storage[0] = 0;
    if (counts[0] == 0) {
        ic_strlcat(tm.storage, "No disks", TM_STORAGE_CAP);
        return;
    }
    tm_cat_u64(tm.storage, (uint64_t)counts[0], TM_STORAGE_CAP);
    ic_strlcat(tm.storage, counts[0] == 1 ? " disk, " : " disks, ", TM_STORAGE_CAP);
    tm_cat_u64(tm.storage, (uint64_t)counts[2], TM_STORAGE_CAP);
    ic_strlcat(tm.storage, " mounted", TM_STORAGE_CAP);
}

static void sample(void) {
    static char buf[TM_BUF_CAP];
    long rc, sn;
    uint64_t wall = 0;

    if (icda_sys_stats(&tm.sys) == 0 && tm.sys.cpus) {
        if (tm.have_prev && tm.sys.uptime_ticks > tm.prev_uptime) {
            uint64_t span = (tm.sys.uptime_ticks - tm.prev_uptime) * tm.sys.cpus;
            uint64_t idle = tm.sys.idle_ticks >= tm.prev_idle ? tm.sys.idle_ticks - tm.prev_idle : 0;
            if (idle > span) idle = span;
            wall = tm.sys.uptime_ticks - tm.prev_uptime;
            tm.cpu_tenths = (uint32_t)((span - idle) * 1000 / span);
            if (tm.history_n == TM_HISTORY) {
                for (int i = 1; i < TM_HISTORY; i++) tm.history[i - 1] = tm.history[i];
                tm.history_n--;
            }
            tm.history[tm.history_n++] = (uint8_t)((tm.cpu_tenths + 5) / 10);
        }
        tm.prev_uptime = tm.sys.uptime_ticks;
        tm.prev_idle = tm.sys.idle_ticks;
        tm.have_prev = 1;
    }

    rc = (long)icda_list_procs(buf, sizeof(buf) - 1);
    if (rc < 0) {
        tm_status("Could not read the process table");
        return;
    }
    buf[rc] = 0;
    parse_procs(buf, (uint64_t)rc, wall);
    sort_procs();
    tm.last_sample = icda_ticks();
    sn = (long)icda_storage_info(tm.storage, sizeof(tm.storage) - 1);
    if (sn < 0 || (uint64_t)sn >= sizeof(tm.storage)) tm.storage[0] = 0;
    else tm.storage[sn] = 0;
    summarize_storage();
}

/* ---- actions -------------------------------------------------------------------- */

static void reselect(uint64_t pid) {
    if (pid == 0) return;
    for (int i = 0; i < tm.count; i++) {
        if (tm.procs[i].pid == pid) { tm.selected = i; return; }
    }
}

static void kill_selected(void) {
    uint64_t pid;
    char name[64];
    if (!has_selection()) return;
    pid = tm.procs[tm.selected].pid;
    tm_copy(name, tm.procs[tm.selected].name, sizeof(name));
    if (icda_kill(pid, 1) == 0) tm_status_name("Quit ", name);
    else tm_status("That process could not be quit");
    sample();
    reselect(pid);
}

static void toggle_suspend(void) {
    uint64_t pid;
    int rc;
    if (!has_selection()) return;
    pid = tm.procs[tm.selected].pid;
    if (tm.procs[tm.selected].suspended) {
        rc = (int)icda_resume(pid);
        tm_status(rc == 0 ? "Process resumed" : "Could not resume that process");
    } else {
        rc = (int)icda_suspend(pid);
        tm_status(rc == 0 ? "Process suspended" : "Could not suspend that process");
    }
    if (rc != 0) return;
    sample();
    reselect(pid);
}

static void confirm(int action) {
    tm.alert_action = action;
    tm.alert_hover = -1;
}

/* ---- drawing -------------------------------------------------------------------- */

static ic_color_t load_tint(uint32_t tenths) {
    const ic_palette_t *p = ic_palette();
    return tenths >= 900 ? p->danger : tenths >= 700 ? p->warning : p->accent;
}

static void draw_toolbar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t b = toolbar_rect(app);
    ic_rect_t k = kill_rect(app);
    const ic_palette_t *p = ic_palette();
    char label[64];

    ic_ui_toolbar(c, b);
    ic_ui_icon_button(c, refresh_rect(app), IC_SYM_RELOAD, tm.hover_refresh ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_button(c, suspend_rect(app), suspend_label_is_resume() ? "Resume" : "Suspend", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 has_selection() ? (tm.hover_suspend ? IC_STATE_HOVER : IC_STATE_NORMAL) : IC_STATE_DISABLED);
    ic_ui_button(c, k, "Quit Process", IC_SYM_NONE, IC_BUTTON_DESTRUCTIVE,
                 has_selection() ? (tm.hover_kill ? IC_STATE_HOVER : IC_STATE_NORMAL) : IC_STATE_DISABLED);
    label[0] = 0;
    if (has_selection()) {
        ic_strlcat(label, tm.procs[tm.selected].name, sizeof(label));
        ic_strlcat(label, " - PID ", sizeof(label));
        tm_cat_u64(label, tm.procs[tm.selected].pid, sizeof(label));
    }
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(k.x + k.w + IC_SP_3, 0, b.w - (k.x + k.w) - IC_SP_4, b.h),
                    label, p->label_secondary, IC_ALIGN_RIGHT);
}

/* caption, big value and a line under it, on the left of a card */
static void card_text(ic_canvas_t *c, ic_rect_t r, const char *caption, const char *value, const char *sub, int w) {
    const ic_palette_t *p = ic_palette();
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make(r.x + IC_SP_4, r.y + IC_SP_3, w, 16), caption,
                    p->label_secondary, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE1), ic_rect_make(r.x + IC_SP_4, r.y + IC_SP_3 + 18, w, 32), value, p->label,
                    IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + IC_SP_4, r.y + r.h - IC_SP_3 - 18, w, 18), sub,
                    p->label_secondary, IC_ALIGN_LEFT);
}

static void draw_cards(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    char value[32], sub[64];

    /* CPU: the number and the last minute */
    {
        ic_rect_t r = card_rect(app, 0), g;
        int text_w = 110;
        ic_ui_group(c, r);
        tm_tenths(value, tm.cpu_tenths, sizeof(value));
        ic_strlcat(value, "%", sizeof(value));
        sub[0] = 0;
        tm_cat_u64(sub, tm.sys.cpus ? tm.sys.cpus : 1, sizeof(sub));
        ic_strlcat(sub, tm.sys.cpus == 1 ? " processor" : " processors", sizeof(sub));
        card_text(c, r, "CPU", tm.have_prev && tm.history_n ? value : "-", sub, text_w);
        g = ic_rect_make(r.x + IC_SP_4 + text_w, r.y + IC_SP_3, r.w - text_w - 2 * IC_SP_4, r.h - 2 * IC_SP_3);
        if (g.w > 40) {
            int bar = g.w / TM_HISTORY;
            ic_gfx_rrect(c, g.x, g.y, g.w, g.h, 6.0f, p->segment_track);
            ic_gfx_hline(c, g.x + 4, g.y + g.h / 2, g.w - 8, IC_RGBA(0x808080, 40));
            if (bar < 1) bar = 1;
            for (int i = 0; i < tm.history_n; i++) {
                int h = (tm.history[i] * (g.h - 6) + 50) / 100;
                int x = g.x + g.w - 3 - (tm.history_n - i) * bar;
                if (h < 1 && tm.history[i]) h = 1;
                if (x < g.x + 3) continue;
                ic_gfx_fill(c, x, g.y + g.h - 3 - h, bar > 2 ? bar - 1 : bar, h, load_tint((uint32_t)tm.history[i] * 10));
            }
        }
    }

    /* memory: used of total, and a bar */
    {
        ic_rect_t r = card_rect(app, 1);
        uint64_t total = tm.sys.mem_total, used = total > tm.sys.mem_free ? total - tm.sys.mem_free : 0;
        uint32_t tenths = total ? (uint32_t)(used * 1000 / total) : 0;
        char total_s[24];
        ic_ui_group(c, r);
        tm_bytes(value, used, sizeof(value));
        tm_bytes(total_s, total, sizeof(total_s));
        sub[0] = 0;
        ic_strlcat(sub, "used of ", sizeof(sub));
        ic_strlcat(sub, total_s, sizeof(sub));
        ic_strlcat(sub, "  (", sizeof(sub));
        tm_cat_u64(sub, (tenths + 5) / 10, sizeof(sub));
        ic_strlcat(sub, "%)", sizeof(sub));
        card_text(c, r, "MEMORY", total ? value : "-", sub, r.w - 2 * IC_SP_4);
        ic_ui_progress(c, ic_rect_make(r.x + IC_SP_4, r.y + IC_SP_3 + 54, r.w - 2 * IC_SP_4, 6), (float)tenths / 1000.0f,
                       load_tint(tenths));
    }

    /* processes and uptime */
    {
        ic_rect_t r = card_rect(app, 2);
        uint64_t s = tm.sys.uptime_ticks / 100, d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
        ic_ui_group(c, r);
        tm_u64((uint64_t)tm.count, value, sizeof(value));
        tm_copy(sub, "Up ", sizeof(sub));
        if (d) { tm_cat_u64(sub, d, sizeof(sub)); ic_strlcat(sub, "d ", sizeof(sub)); }
        if (d || h) { tm_cat_u64(sub, h, sizeof(sub)); ic_strlcat(sub, "h ", sizeof(sub)); }
        tm_cat_u64(sub, m, sizeof(sub));
        ic_strlcat(sub, "m", sizeof(sub));
        card_text(c, r, "PROCESSES", value, sub, r.w - 2 * IC_SP_4);
    }
}

static void draw_table(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t t = table_rect(app);
    const ic_palette_t *pal = ic_palette();
    const ic_face_t *body = ic_font(IC_FONT_BODY);
    const ic_face_t *mono = ic_font(IC_FONT_MONO_SMALL);
    static const char *const names[5] = { "Process", "PID", "State", "CPU %", "Memory" };
    const char *titles[5];
    char marked[5][24];
    int w[5];
    char cell[32];

    layout(app);
    column_widths(app, w);
    for (int i = 0; i < 5; i++) {               /* the sorted column carries an arrow */
        tm_copy(marked[i], names[i], sizeof(marked[i]));
        if (i == tm.sort) ic_strlcat(marked[i], (i == SORT_CPU || i == SORT_MEM) ? " v" : " ^", sizeof(marked[i]));
        titles[i] = marked[i];
    }
    ic_ui_table_header(c, header_rect(app), titles, w, 5);
    ic_gfx_fill(c, t.x, t.y, t.w, t.h, pal->content);

    for (int i = tm.first_row; i < tm.last_row; i++) {
        ic_rect_t r = row_rect(app, i - tm.scroll);
        tm_proc_t *p = &tm.procs[i];
        ic_color_t text = ic_ui_list_row(c, r, i == tm.selected, tm.list_focused, i == tm.hover_row ? 1.0f : 0.0f);
        ic_color_t dim = i == tm.selected ? text : pal->label_secondary;
        int x = r.x + IC_SP_3;

        ic_text_draw_in(c, body, ic_rect_make(x, r.y, w[0] - IC_SP_3 - IC_SP_2, r.h), p->name, text, IC_ALIGN_LEFT);
        x = r.x + w[0];
        tm_u64(p->pid, cell, sizeof(cell));
        ic_text_draw_in(c, mono, ic_rect_make(x, r.y, w[1] - IC_SP_3, r.h), cell, dim, IC_ALIGN_RIGHT);
        x += w[1];
        ic_text_draw_in(c, body, ic_rect_make(x + IC_SP_3, r.y, w[2] - IC_SP_3, r.h), state_label(p),
                        (p->suspended || ic_streq(p->state, "blocked")) ? dim : text, IC_ALIGN_LEFT);
        x += w[2];
        /* CPU: a small bar behind the number */
        if (p->cpu_tenths) {
            int bw = (int)((uint64_t)(w[3] - IC_SP_3) * p->cpu_tenths / 1000);
            if (bw < 2) bw = 2;
            ic_gfx_rrect(c, x + w[3] - IC_SP_3 - bw, r.y + r.h - 6, bw, 3, 1.5f, load_tint(p->cpu_tenths * 2 > 1000 ? 1000 : p->cpu_tenths * 2));
        }
        tm_tenths(cell, p->cpu_tenths, sizeof(cell));
        ic_text_draw_in(c, mono, ic_rect_make(x, r.y, w[3] - IC_SP_3, r.h - 2), cell, p->cpu_tenths ? text : dim,
                        IC_ALIGN_RIGHT);
        x += w[3];
        tm_bytes(cell, p->mem_bytes, sizeof(cell));
        ic_text_draw_in(c, mono, ic_rect_make(x, r.y, w[4] - IC_SP_3, r.h), cell, text, IC_ALIGN_RIGHT);
    }

    if (tm.count == 0) {
        ic_ui_empty_state(c, t, IC_SYM_ACTIVITY, "No processes", "The kernel did not report any running programs.");
    } else if (tm.count > tm.rows) {
        ic_ui_scrollbar(c, t, tm.scroll * IC_H_ROW, tm.count * IC_H_ROW, 1.0f);
    }
}

static void draw_footer(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t f = footer_rect(app);
    const ic_palette_t *p = ic_palette();
    ic_ui_statusbar(c, f, tm.status);
    if (tm.storage[0]) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(f.x + f.w / 2, f.y, f.w / 2 - IC_SP_3, f.h),
                        tm.storage, p->label_secondary, IC_ALIGN_RIGHT);
    }
}

static void draw_alert(ic_app_t *app, ic_canvas_t *c) {
    const char *labels[2] = { "Cancel", tm.alert_action == TM_KILL ? "Quit Process" : "Suspend" };
    ic_rect_t rects[2];
    char msg[160];
    const char *title = tm.alert_action == TM_KILL ? "Quit this process?" : "Suspend this process?";

    msg[0] = 0;
    if (has_selection()) {
        ic_strlcat(msg, tm.procs[tm.selected].name, sizeof(msg));
        ic_strlcat(msg, tm.alert_action == TM_KILL ? " will be stopped and cannot be restarted."
                                                   : " will be paused until it is resumed.", sizeof(msg));
    } else {
        ic_strcpy(msg, "No process is selected.", sizeof(msg));
    }
    ic_ui_alert(c, alert_rect(app), tm.alert_action == TM_KILL ? IC_SYM_WARNING : IC_SYM_INFO, title, msg, labels, 2,
                tm.alert_hover, rects);
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_toolbar(app, c);
    draw_cards(app, c);
    draw_table(app, c);
    draw_footer(app, c);
    if (tm.alert_action != TM_NONE) draw_alert(app, c);
}

/* ---- input -------------------------------------------------------------------- */

static int row_at(ic_app_t *app, int x, int y) {
    ic_rect_t t = table_rect(app);
    int i;
    if (!ic_ui_hit(t, x, y)) return -1;
    i = tm.scroll + (y - t.y) / IC_H_ROW;
    return (i >= 0 && i < tm.count) ? i : -1;
}

static void resolve_alert(void) {
    int action = tm.alert_action;
    tm.alert_action = TM_NONE;
    tm.alert_hover = -1;
    if (action == TM_KILL) kill_selected();
    else if (action == TM_SUSPEND) toggle_suspend();
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        if (tm.alert_action != TM_NONE) {
            tm.alert_hover = ic_ui_hit(alert_button_rect(app, 0), ev->x, ev->y) ? 0
                           : (ic_ui_hit(alert_button_rect(app, 1), ev->x, ev->y) ? 1 : -1);
            break;
        }
        tm.hover_refresh = ic_ui_hit(refresh_rect(app), ev->x, ev->y);
        tm.hover_suspend = ic_ui_hit(suspend_rect(app), ev->x, ev->y);
        tm.hover_kill = ic_ui_hit(kill_rect(app), ev->x, ev->y);
        tm.hover_row = row_at(app, ev->x, ev->y);
        tm.hover_col = column_at(app, ev->x, ev->y);
        break;
    case IC_EV_MOUSE_DOWN:
        if (ev->button != GUI_BTN_LEFT) break;
        if (tm.alert_action != TM_NONE) {
            if (ic_ui_hit(alert_button_rect(app, 1), ev->x, ev->y)) resolve_alert();
            else if (ic_ui_hit(alert_button_rect(app, 0), ev->x, ev->y)) {
                tm.alert_action = TM_NONE;
                tm.alert_hover = -1;
            }
            break;
        }
        if (tm.hover_refresh) { sample(); break; }
        if (tm.hover_suspend && has_selection()) {
            if (suspend_label_is_resume()) toggle_suspend();
            else confirm(TM_SUSPEND);
            break;
        }
        if (tm.hover_kill && has_selection()) { confirm(TM_KILL); break; }
        {
            int col = column_at(app, ev->x, ev->y), i;
            if (col >= 0) {
                tm.sort = col;
                sort_procs();
                break;
            }
            i = row_at(app, ev->x, ev->y);
            if (i >= 0) {
                tm.selected = i;
                tm.list_focused = 1;
            } else {
                tm.list_focused = 0;
            }
        }
        break;
    case IC_EV_MOUSE_LEAVE:
        tm.hover_row = -1;
        tm.hover_col = -1;
        tm.hover_refresh = tm.hover_suspend = tm.hover_kill = 0;
        break;
    case IC_EV_SCROLL:
        tm.scroll += ev->wheel * 3;
        break;
    case IC_EV_KEY:
        if (tm.alert_action != TM_NONE) {
            if (ev->key == IC_KEY_ESCAPE) {
                tm.alert_action = TM_NONE;
                tm.alert_hover = -1;
            } else if (ev->key == IC_KEY_ENTER || ev->key == IC_KEY_RIGHT) {
                resolve_alert();
            } else if (ev->key == IC_KEY_LEFT || ev->key == IC_KEY_TAB) {
                tm.alert_hover = tm.alert_hover == 0 ? 1 : 0;
            }
            break;
        }
        switch (ev->key) {
        case IC_KEY_UP:   if (tm.selected > 0) tm.selected--; break;
        case IC_KEY_DOWN: if (tm.selected + 1 < tm.count) tm.selected++; break;
        case IC_KEY_PAGE_UP:   tm.scroll -= tm.rows; break;
        case IC_KEY_PAGE_DOWN: tm.scroll += tm.rows; break;
        case IC_KEY_HOME:  tm.scroll = 0; break;
        case IC_KEY_END:   tm.scroll = tm.count; break;
        case IC_KEY_DELETE: if (has_selection()) confirm(TM_KILL); break;
        case IC_KEY_ESCAPE: tm.selected = -1; break;
        case 's': case 'S': if (!has_selection()) break;
            if (suspend_label_is_resume()) toggle_suspend();
            else confirm(TM_SUSPEND);
            break;
        case 'r': case 'R': sample(); break;
        default: break;
        }
        /* keep the selection in view */
        if (has_selection()) {
            if (tm.selected < tm.scroll) tm.scroll = tm.selected;
            if (tm.selected >= tm.scroll + tm.rows) tm.scroll = tm.selected - tm.rows + 1;
        }
        break;
    case IC_EV_RESIZE:
        layout(app);
        break;
    case IC_EV_BLUR:
        tm.list_focused = 0;
        break;
    default:
        break;
    }
    ic_app_invalidate(app);
}

/* a sample a second; the picture only changes then (or on input) */
static void tick(ic_app_t *app) {
    if (icda_ticks() - tm.last_sample >= TM_SAMPLE_TICKS) {
        sample();
        ic_app_invalidate(app);
    }
}

static void init(ic_app_t *app) {
    (void)app;
    tm.count = 0;
    tm.selected = -1;
    tm.sort = SORT_CPU;
    tm.scroll = 0;
    tm.hover_row = -1;
    tm.hover_col = -1;
    tm.hover_refresh = tm.hover_suspend = tm.hover_kill = 0;
    tm.list_focused = 1;
    tm.alert_action = TM_NONE;
    tm.alert_hover = -1;
    tm.last_sample = 0;
    tm.status[0] = 0;
    tm.storage[0] = 0;
    sample();
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Activity", WIN_W, WIN_H, init, draw, event, tick };
    (void)argc;
    (void)argv;
    if (ic_app_run(&desc, 0) != 0) {
        icda_write("activity requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
