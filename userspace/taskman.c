/*
 * taskman.app - ICDA Task Manager.
 *
 * Shows every running process with its PID, state, CPU usage (from the
 * scheduler's per-process tick counter) and RAM (mapped user pages),
 * plus storage devices/partitions.  You can select a process and kill it,
 * or refresh the list.  Polls the kernel once a second.
 */
#include "gui.h"
#include "icda_sys.h"
#include "libicda.h"
#include "font.h"

#include <stdint.h>

#define TM_WIN_W 640
#define TM_WIN_H 460

#define TM_MAX_PROCS 64
#define TM_LIST_X 14
#define TM_LIST_Y 66
#define TM_LIST_W (TM_WIN_W - 28)
#define TM_ROW_H 22
#define TM_MAX_ROWS 15
#define TM_STATUS_CAP 128
#define TM_BUF_CAP 8192

typedef struct {
    uint64_t pid;
    char     name[64];
    char     state[16];
    uint64_t cpu_ticks;
    uint64_t mem_bytes;
    uint64_t prev_cpu_ticks;
    uint64_t prev_sample_tick;
} tm_proc_t;

static tm_proc_t procs[TM_MAX_PROCS];
static int proc_count = 0;
static int selected = -1;
static uint64_t last_sample_tick = 0;
static char status[TM_STATUS_CAP];
static char list_buf[TM_BUF_CAP];

static uint64_t tm_strlen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void tm_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void tm_append(char *dst, const char *src, uint64_t cap) {
    uint64_t at = tm_strlen(dst);
    uint64_t i = 0;
    if (!dst || cap == 0 || at >= cap) return;
    while (src && src[i] && at + 1 < cap) {
        dst[at++] = src[i++];
    }
    dst[at] = 0;
}

static void tm_append_uint(char *dst, const char *src_prefix, uint64_t value,
                           const char *src_suffix, uint64_t cap) {
    char num[32];
    uint64_t i = sizeof(num) - 1;
    num[i] = 0;
    if (value == 0) {
        num[--i] = '0';
    } else {
        while (value && i > 0) {
            num[--i] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    if (src_prefix) tm_append(dst, src_prefix, cap);
    tm_append(dst, &num[i], cap);
    if (src_suffix) tm_append(dst, src_suffix, cap);
}

static int tm_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

static uint64_t tm_atoi(const char *s) {
    uint64_t v = 0;
    if (!s) return 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint64_t)(*s - '0');
        s++;
    }
    return v;
}

static void tm_set_status(const char *text) {
    tm_copy(status, text, sizeof(status));
}

/* Parse the SYS_LIST_PROCS text output into the proc table. */
static void tm_parse_procs(const char *buf, uint64_t len) {
    uint64_t pos = 0;
    proc_count = 0;
    while (pos < len && proc_count < TM_MAX_PROCS) {
        char line[256];
        uint64_t li = 0;
        while (pos < len && buf[pos] != '\n' && li + 1 < sizeof(line)) {
            line[li++] = buf[pos++];
        }
        while (pos < len && buf[pos] != '\n') pos++;
        if (pos < len && buf[pos] == '\n') pos++;
        line[li] = 0;

        /* Header line starts with "pid"; skip it. */
        if (li == 0 || (line[0] == 'p' && line[1] == 'i' && line[2] == 'd')) continue;

        /* Fields: pid ppid sid pgid kind state exit */
        uint64_t pid = 0, ppid = 0, sid = 0, pgid = 0;
        char kind[16] = {0}, state[16] = {0};
        uint64_t exit_code = 0;
        int fi = 0;
        char *tok[8];
        char *p = line;
        tok[fi++] = p;
        while (*p && fi < 8) {
            if (*p == ' ') {
                *p = 0;
                p++;
                tok[fi++] = p;
            } else {
                p++;
            }
        }
        if (fi < 7) continue;
        pid = tm_atoi(tok[0]);
        ppid = tm_atoi(tok[1]);
        sid = tm_atoi(tok[2]);
        pgid = tm_atoi(tok[3]);
        tm_copy(kind, tok[4], sizeof(kind));
        tm_copy(state, tok[5], sizeof(state));
        exit_code = tm_atoi(tok[6]);

        (void)ppid; (void)sid; (void)pgid; (void)kind; (void)exit_code;

        procs[proc_count].pid = pid;
        tm_copy(procs[proc_count].name, "?", sizeof(procs[0].name));
        tm_copy(procs[proc_count].state, state, sizeof(procs[0].state));
        procs[proc_count].cpu_ticks = 0;
        procs[proc_count].mem_bytes = 0;
        procs[proc_count].prev_cpu_ticks = 0;
        procs[proc_count].prev_sample_tick = 0;

        /* Per-process detail from the kernel. */
        icda_proc_stats_t st;
        if (icda_proc_stats(pid, &st) == 0) {
            tm_copy(procs[proc_count].name, st.name, sizeof(procs[0].name));
            procs[proc_count].cpu_ticks = st.cpu_ticks;
            procs[proc_count].mem_bytes = st.mem_bytes;
        }
        proc_count++;
    }
    if (selected >= proc_count) selected = -1;
}

static void tm_sample(void) {
    long rc = (long)icda_list_procs(list_buf, sizeof(list_buf));
    if (rc < 0) {
        tm_set_status("Could not read process table");
        return;
    }
    tm_parse_procs(list_buf, (uint64_t)rc);
    last_sample_tick = icda_ticks();
    tm_set_status(proc_count > 0 ? "Refresh: R    Kill: Del/K    Quit: Q" : "(no processes)");
}

static int tm_percent(uint64_t delta_ticks, uint64_t delta_time) {
    if (delta_time == 0) return 0;
    /* One tick = 10ms at the 100Hz scheduler; CPU% = busy ticks / wall ticks. */
    uint64_t pct = delta_ticks * 100 / delta_time;
    if (pct > 999) pct = 999;
    return (int)pct;
}

static void tm_draw_text(int x, int y, const char *text, uint32_t fg, uint32_t bg, int max_px) {
    int cx = x;
    if (max_px <= 0) return;
    while (text && *text && cx + FONT_CELL_WIDTH <= x + max_px) {
        gui_draw_char(cx, y, *text, fg, bg);
        cx += FONT_CELL_WIDTH;
        text++;
    }
}

static void tm_draw_button(int x, int y, int w, int h, const char *label, int active) {
    uint32_t fill = active ? 0x001A73E8 : 0x00E8EAED;
    uint32_t fg = active ? 0x00FFFFFF : 0x00444A50;
    gui_fill_rect(x, y, w, h, fill);
    gui_draw_rect_outline(x, y, w, h, active ? 0x001966C6 : 0x00D0D3D6);
    {
        int cx = x + 8;
        int cy = y + 5;
        const char *s = label;
        while (*s && cx + FONT_CELL_WIDTH <= x + w - 8) {
            gui_draw_char(cx, cy, *s, fg, fill);
            cx += FONT_CELL_WIDTH;
            s++;
        }
    }
}

static void tm_draw(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    int rows = (h - TM_LIST_Y - 92) / TM_ROW_H;
    if (rows > TM_MAX_ROWS) rows = TM_MAX_ROWS;
    if (rows < 1) rows = 1;

    gui_fill_rect(0, 0, w, h, 0x00F5F7FA);
    /* Header bar */
    for (int row = 0; row < 56; row++) {
        gui_fill_rect(0, row, w, 1, 0x001A73E8);
    }
    tm_draw_text(14, 16, "ICDA Task Manager", 0x00FFFFFF, 0x001A73E8, 200);
    tm_draw_text(14, 36, "Refresh: R    Kill: Del/K    Quit: Q", 0x00DCE8FA, 0x001A73E8, w - 28);

    /* Column headers */
    tm_draw_text(TM_LIST_X + 2, TM_LIST_Y - 2, "PID", 0x005F6368, 0x00F5F7FA, 48);
    tm_draw_text(TM_LIST_X + 64, TM_LIST_Y - 2, "Name", 0x005F6368, 0x00F5F7FA, 150);
    tm_draw_text(TM_LIST_X + 230, TM_LIST_Y - 2, "State", 0x005F6368, 0x00F5F7FA, 80);
    tm_draw_text(TM_LIST_X + 320, TM_LIST_Y - 2, "CPU%", 0x005F6368, 0x00F5F7FA, 64);
    tm_draw_text(TM_LIST_X + 380, TM_LIST_Y - 2, "RAM", 0x005F6368, 0x00F5F7FA, 90);

    gui_fill_rect(TM_LIST_X, TM_LIST_Y, TM_LIST_W, rows * TM_ROW_H, 0x00FFFFFF);
    gui_draw_rect_outline(TM_LIST_X, TM_LIST_Y, TM_LIST_W, rows * TM_ROW_H, 0x00D0D3D6);

    {
        uint64_t now = icda_ticks();
        uint64_t elapsed = last_sample_tick ? (now - last_sample_tick) : 0;
        for (int i = 0; i < rows && i < proc_count; i++) {
            int y = TM_LIST_Y + 2 + i * TM_ROW_H;
            tm_proc_t *p = &procs[i];
            if (i == selected) {
                gui_fill_rect(TM_LIST_X + 1, TM_LIST_Y + i * TM_ROW_H, TM_LIST_W - 2, TM_ROW_H - 1, 0x00CFE6FF);
            }
            {
                char cell[64];
                /* PID */
                cell[0] = 0;
                tm_append_uint(cell, 0, p->pid, 0, sizeof(cell));
                tm_draw_text(TM_LIST_X + 4, y, cell, i == selected ? 0x001F2937 : 0x001F2937,
                             i == selected ? 0x00CFE6FF : 0x00FFFFFF, 48);
                /* Name */
                tm_draw_text(TM_LIST_X + 64, y, p->name,
                             i == selected ? 0x001F2937 : 0x001F2937,
                             i == selected ? 0x00CFE6FF : 0x00FFFFFF, 150);
                /* State */
                tm_draw_text(TM_LIST_X + 230, y, p->state,
                             i == selected ? 0x001F2937 : 0x0064758B,
                             i == selected ? 0x00CFE6FF : 0x00FFFFFF, 80);
                /* CPU% (delta since last sample) */
                uint64_t dticks = p->cpu_ticks > p->prev_cpu_ticks ? p->cpu_ticks - p->prev_cpu_ticks : 0;
                cell[0] = 0;
                tm_append_uint(cell, 0, (uint64_t)tm_percent(dticks, elapsed), 0, sizeof(cell));
                tm_draw_text(TM_LIST_X + 322, y, cell,
                             i == selected ? 0x001F2937 : 0x00334455,
                             i == selected ? 0x00CFE6FF : 0x00FFFFFF, 48);
                /* RAM */
                cell[0] = 0;
                tm_append_uint(cell, 0, p->mem_bytes / 1024, "K", sizeof(cell));
                tm_draw_text(TM_LIST_X + 382, y, cell,
                             i == selected ? 0x001F2937 : 0x00334455,
                             i == selected ? 0x00CFE6FF : 0x00FFFFFF, 90);
                p->prev_cpu_ticks = p->cpu_ticks;
            }
        }
    }

    if (proc_count == 0) {
        tm_draw_text(TM_LIST_X + 8, TM_LIST_Y + 6, "(no processes)", 0x0064758B, 0x00FFFFFF, TM_LIST_W - 20);
    }

    /* Footer: storage info */
    gui_fill_rect(0, h - 84, w, 84, 0x00E8ECF1);
    gui_draw_hline(0, h - 84, w, 0x00D0D3D6);
    tm_draw_text(12, h - 78, "Storage:", 0x005F6368, 0x00E8ECF1, 100);
    {
        long n = (long)icda_storage_info(list_buf, sizeof(list_buf));
        int y = h - 62;
        if (n > 0) {
            tm_draw_text(12, y, list_buf, 0x00334455, 0x00E8ECF1, w - 24);
        } else {
            tm_draw_text(12, y, "(no storage info)", 0x0064758B, 0x00E8ECF1, w - 24);
        }
    }

    tm_draw_button(12, h - 36, 80, 26, "Kill", selected >= 0);
    tm_draw_button(100, h - 36, 90, 26, "Refresh", 1);
    tm_draw_button(198, h - 36, 70, 26, "Quit", 1);
    tm_draw_text(290, h - 30, status, 0x00334455, 0x00E8ECF1, w - 300);
}

static void tm_kill_selected(void) {
    if (selected < 0 || selected >= proc_count) {
        tm_set_status("Nothing selected");
        return;
    }
    if (icda_kill(procs[selected].pid, 1) == 0) {
        char msg[TM_STATUS_CAP];
        tm_copy(msg, "Killed PID ", sizeof(msg));
        tm_append_uint(msg, 0, procs[selected].pid, 0, sizeof(msg));
        tm_set_status(msg);
    } else {
        tm_set_status("Kill failed (protected process?)");
    }
    tm_sample();
}

static void tm_handle_mouse(gui_msg_t *msg) {
    int mx = msg->mouse.x;
    int my = msg->mouse.y;
    int h = gui_window_height();
    int rows = (h - TM_LIST_Y - 92) / TM_ROW_H;
    if (rows > TM_MAX_ROWS) rows = TM_MAX_ROWS;
    if (rows < 1) rows = 1;

    if (tm_hit(mx, my, 12, h - 36, 80, 26)) { tm_kill_selected(); return; }
    if (tm_hit(mx, my, 100, h - 36, 90, 26)) { tm_sample(); return; }
    if (tm_hit(mx, my, 198, h - 36, 70, 26)) { gui_close_window(); icda_exit(0); return; }

    if (tm_hit(mx, my, TM_LIST_X, TM_LIST_Y, TM_LIST_W, rows * TM_ROW_H)) {
        int i = (my - TM_LIST_Y) / TM_ROW_H;
        if (i >= 0 && i < proc_count) selected = i;
        return;
    }
}

static void tm_handle_key(uint32_t key) {
    if (key == 24 || key == 'q' || key == 'Q') {
        gui_close_window();
        icda_exit(0);
        return;
    }
    if (key == 3) { /* SPECIAL_UP */ if (selected > 0) selected--; return; }
    if (key == 4) { /* SPECIAL_DOWN */ if (selected + 1 < proc_count) selected++; return; }
    if (key == 'r' || key == 'R') { tm_sample(); return; }
    if (key == 'k' || key == 'K' || key == 127) { tm_kill_selected(); return; }
}

int main(int argc, char **argv) {
    int key_seq = 0;
    (void)argc;
    (void)argv;

    status[0] = 0;
    if (gui_open_window("Task Manager", TM_WIN_W, TM_WIN_H) != 0) {
        icda_write("task manager requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    tm_sample();
    tm_draw();
    gui_flush();

    for (;;) {
        gui_msg_t msg;
        int changed = 0;
        while (gui_poll_event(&msg)) {
            changed = 1;
            if (msg.type == GUI_MSG_MOUSE_EVENT && (msg.mouse.buttons & GUI_BTN_LEFT)) {
                tm_handle_mouse(&msg);
            } else if (msg.type == GUI_MSG_KEY_EVENT && msg.key.pressed) {
                uint32_t code = msg.key.keycode;
                if (key_seq == 0 && code == 27) {
                    key_seq = 1;
                } else if (key_seq == 1 && code == '[') {
                    key_seq = 2;
                } else if (key_seq == 2) {
                    key_seq = 0;
                    if (code == 'A') tm_handle_key(3);
                    else if (code == 'B') tm_handle_key(4);
                } else {
                    key_seq = 0;
                    tm_handle_key(code);
                }
            } else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                gui_close_window();
                return 0;
            }
        }
        /* Auto-refresh every 100 ticks (~1s) so CPU% and RAM stay live. */
        if (changed || (icda_ticks() - last_sample_tick) > 100) {
            tm_sample();
            tm_draw();
            gui_flush();
        }
        icda_sleep(1);
    }
}
