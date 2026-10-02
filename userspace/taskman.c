












#include "libicda.h"

#define WIN_W 720
#define WIN_H 480

#define TM_MAX_PROCS     64
#define TM_BUF_CAP       8192
#define TM_STATUS_CAP    160
#define TM_STORAGE_CAP   512
#define TM_SAMPLE_TICKS  100      
#define TM_FOOTER_H      26


#define COL_PID    0
#define COL_NAME   58
#define COL_STATE  238
#define COL_CPU    312
#define COL_MEM    384
#define COL_MEM_W  92

enum { TM_NONE = 0, TM_KILL, TM_SUSPEND };

typedef struct {
    uint64_t pid;
    char     name[64];
    char     state[16];
    uint64_t cpu_ticks;
    uint64_t mem_bytes;
    uint64_t prev_cpu_ticks;
    int      suspended;           
} tm_proc_t;

static struct {
    tm_proc_t procs[TM_MAX_PROCS];
    int       count;
    int       selected;

    
    int rows;
    int scroll;
    int first_row;                
    int last_row;                 

    
    int hover_row;
    int hover_refresh;
    int hover_suspend;
    int hover_kill;
    int list_focused;

    
    int alert_action;             
    int alert_hover;              

    char     storage[TM_STORAGE_CAP];
    char     status[TM_STATUS_CAP];
    uint64_t last_sample;
} tm;



static ic_rect_t toolbar_rect(ic_app_t *app) {
    return ic_rect_make(0, 0, app->width, IC_H_TOOLBAR);
}

static ic_rect_t table_rect(ic_app_t *app) {
    int y = IC_H_TOOLBAR + IC_H_ROW + IC_SP_1;
    return ic_rect_make(IC_SP_4, y, app->width - 2 * IC_SP_4,
                        app->height - y - TM_FOOTER_H - IC_SP_2);
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



static void layout(ic_app_t *app) {
    ic_rect_t t = table_rect(app);
    int rows = t.h / IC_H_ROW;
    if (rows < 1) rows = 1;
    if (rows > TM_MAX_PROCS) rows = TM_MAX_PROCS;
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
    return tm.selected >= 0 && tm.selected < tm.count && tm.procs[tm.selected].suspended;
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
    int w = 380, h = 140;
    return ic_rect_make((app->width - w) / 2, (app->height - h) / 2, w, h);
}

static ic_rect_t alert_button_rect(ic_app_t *app, int index) {
    ic_rect_t r = alert_rect(app);
    const char *label = index == 0 ? "Cancel" : "Quit Process";
    int w = ic_ui_button_width(label, IC_SYM_NONE);
    int y = r.y + r.h - IC_H_CONTROL - IC_SP_3;
    return ic_rect_make(r.x + r.w - w - (index == 0 ? w + IC_SP_2 + IC_SP_3 : IC_SP_3), y, w,
                        IC_H_CONTROL);
}



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

static void tm_status(const char *text) {
    tm_copy(tm.status, text, TM_STATUS_CAP);
}

static void tm_status_pid(const char *prefix, uint64_t pid) {
    char digits[24];
    tm_u64(pid, digits, sizeof(digits));
    tm.status[0] = 0;
    ic_strlcat(tm.status, prefix, TM_STATUS_CAP);
    ic_strlcat(tm.status, digits, TM_STATUS_CAP);
}

static int tm_atoi(const char *s) {
    uint64_t v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
    return (int)v;
}

static int cpu_percent(uint64_t busy, uint64_t wall) {
    uint64_t pct;
    if (wall == 0) return 0;
    pct = busy * 100 / wall;
    if (pct > 999) pct = 999;
    return (int)pct;
}

static const char *state_label(int i) {
    const tm_proc_t *p = &tm.procs[i];
    if (p->suspended) return "Suspended";
    if (ic_streq(p->state, "R")) return "Running";
    if (ic_streq(p->state, "S")) return "Sleeping";
    if (ic_streq(p->state, "Z")) return "Stopped";
    return p->state;
}

static int state_is_idle(int i) {
    const char *s = state_label(i);
    return s[0] == 'S' || s[0] == 'Z';
}

static int has_selection(void) {
    return tm.selected >= 0 && tm.selected < tm.count;
}



static void parse_procs(const char *buf, uint64_t len) {
    uint64_t pos = 0;
    tm.count = 0;
    while (pos < len && tm.count < TM_MAX_PROCS) {
        char line[256];
        uint64_t li = 0;
        char *tok[8];
        char *p;
        int fi = 0;
        int pid;
        tm_proc_t *out;

        while (pos < len && buf[pos] != '\n' && li + 1 < sizeof(line)) {
            line[li++] = buf[pos++];
        }
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

        out = &tm.procs[tm.count];
        out->pid = (uint64_t)pid;
        tm_copy(out->name, "?", sizeof(out->name));
        tm_copy(out->state, tok[5], sizeof(out->state));
        out->cpu_ticks = 0;
        out->mem_bytes = 0;
        out->prev_cpu_ticks = 0;
        out->suspended = 0;
        
        for (int k = 0; k < tm.count; k++) {
            if (tm.procs[k].pid == out->pid && tm.procs[k].suspended) out->suspended = 1;
        }
        {
            icda_proc_stats_t st;
            if (icda_proc_stats((uint64_t)pid, &st) == 0) {
                tm_copy(out->name, st.name, sizeof(out->name));
                out->cpu_ticks = st.cpu_ticks;
                out->mem_bytes = st.mem_bytes;
            }
        }
        tm.count++;
    }
    if (tm.selected >= tm.count) tm.selected = -1;
}

static void sample(void) {
    static char buf[TM_BUF_CAP];
    long rc = (long)icda_list_procs(buf, sizeof(buf) - 1);
    long sn;
    if (rc < 0) {
        tm_status("Could not read the process table");
        return;
    }
    buf[rc] = 0;
    parse_procs(buf, (uint64_t)rc);
    tm.last_sample = icda_ticks();
    sn = (long)icda_storage_info(tm.storage, sizeof(tm.storage) - 1);
    if (sn < 0 || (uint64_t)sn >= sizeof(tm.storage)) tm.storage[0] = 0;
    else tm.storage[sn] = 0;
}

static uint64_t selected_pid(void) {
    return has_selection() ? tm.procs[tm.selected].pid : 0;
}

static void reselect(uint64_t pid) {
    if (pid == 0) return;
    for (int i = 0; i < tm.count; i++) {
        if (tm.procs[i].pid == pid) { tm.selected = i; return; }
    }
}



static void kill_selected(void) {
    uint64_t pid;
    if (!has_selection()) return;
    pid = tm.procs[tm.selected].pid;
    if (icda_kill(pid, 1) == 0) {
        tm_status_pid("Quit PID ", pid);
    } else {
        tm_status("That process could not be quit");
    }
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



static void draw_toolbar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t b = toolbar_rect(app);
    ic_rect_t k = kill_rect(app);
    const ic_palette_t *p = ic_palette();
    char label[64];
    int n = tm.count;

    ic_ui_toolbar(c, b);
    ic_ui_icon_button(c, refresh_rect(app), IC_SYM_RELOAD,
                      tm.hover_refresh ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_button(c, suspend_rect(app), suspend_label_is_resume() ? "Resume" : "Suspend",
                 IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 has_selection() ? (tm.hover_suspend ? IC_STATE_HOVER : IC_STATE_NORMAL)
                                 : IC_STATE_DISABLED);
    ic_ui_button(c, k, "Quit Process", IC_SYM_NONE, IC_BUTTON_DESTRUCTIVE,
                 has_selection() ? (tm.hover_kill ? IC_STATE_HOVER : IC_STATE_NORMAL)
                                 : IC_STATE_DISABLED);

    if (n > 0) {
        tm_u64((uint64_t)n, label, 24);
        ic_strlcat(label, n == 1 ? " process" : " processes", sizeof(label));
    } else {
        ic_strcpy(label, "No processes", sizeof(label));
    }
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                    ic_rect_make(k.x + k.w + IC_SP_3, 0,
                                 b.w - (k.x + k.w) - IC_SP_3, b.h),
                    label, p->label_secondary, IC_ALIGN_LEFT);
}

static void draw_table(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t t = table_rect(app);
    const ic_face_t *body = ic_font(IC_FONT_BODY);
    const ic_face_t *mono = ic_font(IC_FONT_MONO_SMALL);
    static const char *const titles[5] = { "PID", "Process", "State", "CPU", "Memory" };
    static const int widths[5] = { COL_NAME, COL_STATE - COL_NAME, COL_CPU - COL_STATE,
                                   COL_MEM - COL_CPU, COL_MEM_W + IC_SP_4 };
    char cell[32];

    layout(app);
    ic_ui_table_header(c, header_rect(app), titles, widths, 5);
    ic_gfx_fill(c, t.x, t.y, t.w, t.h, ic_palette()->content);

    for (int i = tm.first_row; i < tm.last_row; i++) {
        ic_rect_t r = row_rect(app, i - tm.scroll);
        tm_proc_t *p = &tm.procs[i];
        ic_color_t text = ic_ui_list_row(c, r, i == tm.selected, tm.list_focused,
                                         i == tm.hover_row ? 1.0f : 0.0f);
        uint64_t busy = p->cpu_ticks > p->prev_cpu_ticks ? p->cpu_ticks - p->prev_cpu_ticks : 0;
        
        uint64_t wall = TM_SAMPLE_TICKS;

        tm_u64(p->pid, cell, sizeof(cell));
        ic_text_draw_in(c, mono, ic_rect_make(r.x + COL_PID, r.y, COL_NAME - COL_PID - IC_SP_2, r.h),
                        cell, text, IC_ALIGN_LEFT);
        ic_text_draw_in(c, body, ic_rect_make(r.x + COL_NAME, r.y, COL_STATE - COL_NAME - IC_SP_2, r.h),
                        p->name, text, IC_ALIGN_LEFT);
        ic_text_draw_in(c, body, ic_rect_make(r.x + COL_STATE, r.y, COL_CPU - COL_STATE - IC_SP_2, r.h),
                        state_label(i), state_is_idle(i) ? ic_palette()->label_secondary : text,
                        IC_ALIGN_LEFT);
        tm_u64((uint64_t)cpu_percent(busy, wall), cell, sizeof(cell));
        ic_text_draw_in(c, mono, ic_rect_make(r.x + COL_CPU, r.y, COL_MEM - COL_CPU - IC_SP_2, r.h),
                        cell, text, IC_ALIGN_RIGHT);
        tm_u64(p->mem_bytes / 1024, cell, sizeof(cell));
        ic_text_draw_in(c, mono, ic_rect_make(r.x + COL_MEM, r.y, COL_MEM_W, r.h),
                        cell, text, IC_ALIGN_RIGHT);
        
        p->prev_cpu_ticks = p->cpu_ticks;
    }

    if (tm.count == 0) {
        ic_ui_empty_state(c, t, IC_SYM_ACTIVITY, "No processes",
                          "The kernel did not report any running programs.");
    } else if (tm.count > tm.rows) {
        ic_ui_scrollbar(c, t, tm.scroll, tm.count, 1.0f);
    }
}

static void draw_footer(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t f = footer_rect(app);
    const ic_palette_t *p = ic_palette();
    ic_ui_statusbar(c, f, tm.status);
    if (tm.storage[0]) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(f.x + f.w / 2, f.y, f.w / 2 - IC_SP_3, f.h),
                        tm.storage, p->label_secondary, IC_ALIGN_RIGHT);
    }
}

static void draw_alert(ic_app_t *app, ic_canvas_t *c) {
    static const char *const labels[2] = { "Cancel", "Quit Process" };
    ic_rect_t rects[2];
    char msg[160];
    const char *title = tm.alert_action == TM_KILL ? "Quit this process?" : "Suspend this process?";

    msg[0] = 0;
    if (has_selection()) {
        ic_strlcat(msg, tm.procs[tm.selected].name, sizeof(msg));
        if (tm.alert_action == TM_KILL) {
            ic_strlcat(msg, " will be stopped and cannot be restarted.", sizeof(msg));
        } else {
            ic_strlcat(msg, " will be paused until it is resumed.", sizeof(msg));
        }
    } else {
        ic_strcpy(msg, "No process is selected.", sizeof(msg));
    }
    ic_ui_alert(c, alert_rect(app), tm.alert_action == TM_KILL ? IC_SYM_WARNING : IC_SYM_INFO,
                title, msg, labels, 2, tm.alert_hover, rects);
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_toolbar(app, c);
    draw_table(app, c);
    draw_footer(app, c);
    if (tm.alert_action != TM_NONE) draw_alert(app, c);
    
    if (app->focused) ic_app_animate(app);
}



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
        if (tm.hover_suspend && has_selection()) { confirm(TM_SUSPEND); break; }
        if (tm.hover_kill && has_selection()) { confirm(TM_KILL); break; }
        {
            int i = row_at(app, ev->x, ev->y);
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
        tm.hover_refresh = tm.hover_suspend = tm.hover_kill = 0;
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
        case IC_KEY_ESCAPE: if (has_selection()) confirm(TM_KILL); break;
        case 's': case 'S': if (has_selection()) confirm(TM_SUSPEND); break;
        case 'r': case 'R': sample(); break;
        default: break;
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

static void tick(ic_app_t *app) {
    if (icda_ticks() - tm.last_sample > TM_SAMPLE_TICKS) {
        uint64_t pid = selected_pid();
        sample();
        reselect(pid);
        ic_app_invalidate(app);
    }
}

static void init(ic_app_t *app) {
    (void)app;
    tm.count = 0;
    tm.selected = -1;
    tm.scroll = 0;
    tm.hover_row = -1;
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
