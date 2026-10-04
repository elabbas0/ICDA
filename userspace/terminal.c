#include "libicda.h"

#define WIN_W 640
#define WIN_H 420

#define TERM_PAD      IC_SP_2
#define TERM_MAX_COLS 240
#define TERM_MAX_ROWS 120
#define TERM_HISTORY  2000
#define TERM_SHELL    "/apps/shell.app"

enum { ATTR_BOLD = 1, ATTR_INVERSE = 2, ATTR_UNDERLINE = 4 };

typedef struct {
    char    ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
} term_cell_t;

typedef struct {
    term_cell_t cells[TERM_MAX_COLS];
} term_line_t;

enum { PS_NORMAL = 0, PS_ESC, PS_CSI, PS_OSC, PS_OSC_ESC, PS_CHARSET };

static struct {
    term_line_t lines[TERM_HISTORY];
    int         head;
    int         count;
    int         cols, rows;
    int         cx, cy;
    int         saved_cx, saved_cy;
    int         wrap_pending;
    uint8_t     fg, bg, attr;
    int         cursor_visible;
    int         scroll_top, scroll_bot;

    int         state;
    int         params[16];
    int         nparams;
    int         private_mode;

    long        pty;
    uint64_t    pid;
    int         exited;
    int         view_offset;
    uint64_t    last_alive_check;

    int         hover_scroll;
    int         dragging_scroll;
    ic_tween_t  scrollbar;
} term;

#define DEFAULT_FG 7
#define DEFAULT_BG 0

static const ic_face_t *mono(void) { return ic_font(IC_FONT_MONO); }

static int cell_w(void) {
    int w = ic_text_measure(mono(), "0");
    return w > 0 ? w : 8;
}

static int cell_h(void) {
    int h = mono()->line_h;
    return h > 0 ? h : 16;
}

static ic_rect_t content_rect(ic_app_t *app) {
    return ic_rect_make(TERM_PAD, TERM_PAD, app->width - 2 * TERM_PAD, app->height - 2 * TERM_PAD);
}

static term_line_t *line_at(int index_from_oldest) {
    int first = (term.head - term.count + TERM_HISTORY) % TERM_HISTORY;
    return &term.lines[(first + index_from_oldest) % TERM_HISTORY];
}

static term_line_t *screen_line(int row) {
    return line_at(term.count - term.rows + row);
}

static void clear_cells(term_cell_t *c, int n) {
    for (int i = 0; i < n; i++) {
        c[i].ch = ' ';
        c[i].fg = term.fg;
        c[i].bg = term.bg;
        c[i].attr = 0;
    }
}

static void push_line(void) {
    term_line_t *l = &term.lines[term.head];
    term.head = (term.head + 1) % TERM_HISTORY;
    if (term.count < TERM_HISTORY) term.count++;
    clear_cells(l->cells, TERM_MAX_COLS);
}

static void term_reset_screen(void) {
    term.head = 0;
    term.count = 0;
    term.fg = DEFAULT_FG;
    term.bg = DEFAULT_BG;
    term.attr = 0;
    for (int i = 0; i < term.rows; i++) push_line();
    term.cx = term.cy = 0;
    term.wrap_pending = 0;
    term.scroll_top = 0;
    term.scroll_bot = term.rows - 1;
    term.cursor_visible = 1;
}

static void scroll_up_region(int top, int bot) {
    if (top == 0 && bot == term.rows - 1) {
        push_line();
        return;
    }
    for (int r = top; r < bot; r++) *screen_line(r) = *screen_line(r + 1);
    clear_cells(screen_line(bot)->cells, TERM_MAX_COLS);
}

static void scroll_down_region(int top, int bot) {
    for (int r = bot; r > top; r--) *screen_line(r) = *screen_line(r - 1);
    clear_cells(screen_line(top)->cells, TERM_MAX_COLS);
}

static void line_feed(void) {
    if (term.cy == term.scroll_bot) scroll_up_region(term.scroll_top, term.scroll_bot);
    else if (term.cy < term.rows - 1) term.cy++;
}

static void put_char(char ch) {
    term_cell_t *c;
    if (term.wrap_pending) {
        term.cx = 0;
        line_feed();
        term.wrap_pending = 0;
    }
    c = &screen_line(term.cy)->cells[term.cx];
    c->ch = ch;
    c->fg = term.fg;
    c->bg = term.bg;
    c->attr = term.attr;
    if (term.cx == term.cols - 1) term.wrap_pending = 1;
    else term.cx++;
}

static int param(int i, int def) {
    return (i < term.nparams && term.params[i] > 0) ? term.params[i] : def;
}

static void clamp_cursor(void) {
    if (term.cx < 0) term.cx = 0;
    if (term.cx >= term.cols) term.cx = term.cols - 1;
    if (term.cy < 0) term.cy = 0;
    if (term.cy >= term.rows) term.cy = term.rows - 1;
    term.wrap_pending = 0;
}

static void erase_line_part(int row, int from, int to) {
    term_cell_t *c = screen_line(row)->cells;
    if (from < 0) from = 0;
    if (to > TERM_MAX_COLS) to = TERM_MAX_COLS;
    if (to > from) clear_cells(c + from, to - from);
}

static void sgr(void) {
    if (term.nparams == 0) {
        term.fg = DEFAULT_FG;
        term.bg = DEFAULT_BG;
        term.attr = 0;
        return;
    }
    for (int i = 0; i < term.nparams; i++) {
        int p = term.params[i];
        if (p == 0) { term.fg = DEFAULT_FG; term.bg = DEFAULT_BG; term.attr = 0; }
        else if (p == 1) term.attr |= ATTR_BOLD;
        else if (p == 4) term.attr |= ATTR_UNDERLINE;
        else if (p == 7) term.attr |= ATTR_INVERSE;
        else if (p == 22) term.attr &= (uint8_t)~ATTR_BOLD;
        else if (p == 24) term.attr &= (uint8_t)~ATTR_UNDERLINE;
        else if (p == 27) term.attr &= (uint8_t)~ATTR_INVERSE;
        else if (p >= 30 && p <= 37) term.fg = (uint8_t)(p - 30);
        else if (p == 39) term.fg = DEFAULT_FG;
        else if (p >= 40 && p <= 47) term.bg = (uint8_t)(p - 40);
        else if (p == 49) term.bg = DEFAULT_BG;
        else if (p >= 90 && p <= 97) term.fg = (uint8_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107) term.bg = (uint8_t)(p - 100 + 8);
        else if ((p == 38 || p == 48) && i + 2 < term.nparams && term.params[i + 1] == 5) {
            int v = term.params[i + 2];
            uint8_t idx = (uint8_t)(v < 16 ? v : 7);
            if (p == 38) term.fg = idx;
            else term.bg = idx;
            i += 2;
        }
    }
}

static void csi_dispatch(char f) {
    int n = param(0, 1);
    switch (f) {
    case 'A': term.cy -= n; clamp_cursor(); break;
    case 'B': term.cy += n; clamp_cursor(); break;
    case 'C': term.cx += n; clamp_cursor(); break;
    case 'D': term.cx -= n; clamp_cursor(); break;
    case 'E': term.cy += n; term.cx = 0; clamp_cursor(); break;
    case 'F': term.cy -= n; term.cx = 0; clamp_cursor(); break;
    case 'G': term.cx = n - 1; clamp_cursor(); break;
    case 'd': term.cy = n - 1; clamp_cursor(); break;
    case 'H':
    case 'f':
        term.cy = param(0, 1) - 1;
        term.cx = param(1, 1) - 1;
        clamp_cursor();
        break;
    case 'J': {
        int mode = term.nparams ? term.params[0] : 0;
        if (mode == 0) {
            erase_line_part(term.cy, term.cx, TERM_MAX_COLS);
            for (int r = term.cy + 1; r < term.rows; r++) erase_line_part(r, 0, TERM_MAX_COLS);
        } else if (mode == 1) {
            erase_line_part(term.cy, 0, term.cx + 1);
            for (int r = 0; r < term.cy; r++) erase_line_part(r, 0, TERM_MAX_COLS);
        } else {
            for (int r = 0; r < term.rows; r++) erase_line_part(r, 0, TERM_MAX_COLS);
        }
        break;
    }
    case 'K': {
        int mode = term.nparams ? term.params[0] : 0;
        if (mode == 0) erase_line_part(term.cy, term.cx, TERM_MAX_COLS);
        else if (mode == 1) erase_line_part(term.cy, 0, term.cx + 1);
        else erase_line_part(term.cy, 0, TERM_MAX_COLS);
        break;
    }
    case 'P': {
        term_cell_t *c = screen_line(term.cy)->cells;
        for (int x = term.cx; x < term.cols; x++) {
            if (x + n < term.cols) c[x] = c[x + n];
            else clear_cells(&c[x], 1);
        }
        break;
    }
    case '@': {
        term_cell_t *c = screen_line(term.cy)->cells;
        for (int x = term.cols - 1; x >= term.cx + n; x--) c[x] = c[x - n];
        for (int x = term.cx; x < term.cx + n && x < term.cols; x++) clear_cells(&c[x], 1);
        break;
    }
    case 'L': for (int i = 0; i < n; i++) scroll_down_region(term.cy, term.scroll_bot); break;
    case 'M': for (int i = 0; i < n; i++) scroll_up_region(term.cy, term.scroll_bot); break;
    case 'S': for (int i = 0; i < n; i++) scroll_up_region(term.scroll_top, term.scroll_bot); break;
    case 'T': for (int i = 0; i < n; i++) scroll_down_region(term.scroll_top, term.scroll_bot); break;
    case 'm': sgr(); break;
    case 's': term.saved_cx = term.cx; term.saved_cy = term.cy; break;
    case 'u': term.cx = term.saved_cx; term.cy = term.saved_cy; clamp_cursor(); break;
    case 'r':
        term.scroll_top = param(0, 1) - 1;
        term.scroll_bot = param(1, term.rows) - 1;
        if (term.scroll_top < 0 || term.scroll_bot >= term.rows || term.scroll_top >= term.scroll_bot) {
            term.scroll_top = 0;
            term.scroll_bot = term.rows - 1;
        }
        term.cx = term.cy = 0;
        break;
    case 'h':
    case 'l':
        if (term.private_mode && param(0, 0) == 25) term.cursor_visible = f == 'h';
        break;
    default:
        break;
    }
}

static void feed(char ch) {
    unsigned char c = (unsigned char)ch;
    switch (term.state) {
    case PS_ESC:
        term.state = PS_NORMAL;
        if (c == '[') {
            term.state = PS_CSI;
            term.nparams = 0;
            term.private_mode = 0;
            for (int i = 0; i < 16; i++) term.params[i] = 0;
        } else if (c == ']') {
            term.state = PS_OSC;
        } else if (c == '(' || c == ')') {
            term.state = PS_CHARSET;
        } else if (c == '7') {
            term.saved_cx = term.cx;
            term.saved_cy = term.cy;
        } else if (c == '8') {
            term.cx = term.saved_cx;
            term.cy = term.saved_cy;
            clamp_cursor();
        } else if (c == 'c') {
            term_reset_screen();
        } else if (c == 'D') {
            line_feed();
        } else if (c == 'M') {
            if (term.cy == term.scroll_top) scroll_down_region(term.scroll_top, term.scroll_bot);
            else if (term.cy > 0) term.cy--;
        } else if (c == 'E') {
            term.cx = 0;
            line_feed();
        }
        return;
    case PS_CHARSET:
        term.state = PS_NORMAL;
        return;
    case PS_OSC:
        if (c == 7) term.state = PS_NORMAL;
        else if (c == 27) term.state = PS_OSC_ESC;
        return;
    case PS_OSC_ESC:
        term.state = PS_NORMAL;
        return;
    case PS_CSI:
        if (c == '?') {
            term.private_mode = 1;
        } else if (c >= '0' && c <= '9') {
            if (term.nparams == 0) term.nparams = 1;
            term.params[term.nparams - 1] = term.params[term.nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (term.nparams == 0) term.nparams = 1;
            if (term.nparams < 16) term.nparams++;
        } else if (c >= 0x40 && c <= 0x7E) {
            term.state = PS_NORMAL;
            csi_dispatch((char)c);
        }
        return;
    default:
        break;
    }
    switch (c) {
    case 27: term.state = PS_ESC; break;
    case '\r': term.cx = 0; term.wrap_pending = 0; break;
    case '\n':
    case 11:
    case 12:
        term.cx = 0;
        term.wrap_pending = 0;
        line_feed();
        break;
    case '\b':
        if (term.cx > 0) term.cx--;
        term.wrap_pending = 0;
        break;
    case '\t': {
        int next = (term.cx / 8 + 1) * 8;
        term.cx = next < term.cols ? next : term.cols - 1;
        break;
    }
    case 7:
        break;
    default:
        if (c >= 32 && c != 127) put_char(c < 128 ? (char)c : '?');
        break;
    }
}

static void feed_str(const char *s) {
    while (*s) feed(*s++);
}

static void pty_send(const char *s, int n) {
    if (term.pty > 0 && !term.exited) icda_pty_io(term.pty, ICDA_PTY_WRITE, (void *)s, (uint64_t)n);
}

static void pty_send_str(const char *s) {
    pty_send(s, (int)ic_strlen(s));
}

static void layout(ic_app_t *app) {
    ic_rect_t r = content_rect(app);
    int cols = (r.w - 10) / cell_w();
    int rows = r.h / cell_h();
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;
    if (cols == term.cols && rows == term.rows) return;
    if (term.count == 0) {
        term.cols = cols;
        term.rows = rows;
        term_reset_screen();
    } else {
        while (term.count < rows) push_line();
        if (term.cy >= rows) term.cy = rows - 1;
        term.cols = cols;
        term.rows = rows;
        if (term.cx >= cols) term.cx = cols - 1;
        term.scroll_top = 0;
        term.scroll_bot = rows - 1;
    }
    if (term.pty > 0) icda_pty_io(term.pty, ICDA_PTY_RESIZE, 0, (uint64_t)cols | ((uint64_t)rows << 16));
}

static ic_color_t ansi_color(int idx, int is_fg) {
    static const uint32_t dark[16] = {
        0x1E1E1E, 0xF2555A, 0x4DD07A, 0xE5C07B, 0x5AA9FF, 0xC678DD, 0x56C8D8, 0xD4D4D8,
        0x6B6B70, 0xFF7B80, 0x7EE6A1, 0xF5D58B, 0x8CC4FF, 0xDDA0F0, 0x8AE2EE, 0xFFFFFF
    };
    static const uint32_t light[16] = {
        0xFFFFFF, 0xC0262D, 0x188038, 0x986801, 0x1A62D6, 0x8E37B3, 0x0E7C86, 0x1D1D1F,
        0x86868B, 0xE0434A, 0x26A148, 0xB98A10, 0x2F7BF0, 0xA855D0, 0x1597A3, 0x000000
    };
    const ic_palette_t *p = ic_palette();
    if (is_fg && idx == DEFAULT_FG) return p->label;
    if (!is_fg && idx == DEFAULT_BG) return p->content;
    return IC_RGB(p->dark ? dark[idx & 15] : light[idx & 15]);
}

static int max_view_offset(void) {
    int m = term.count - term.rows;
    return m > 0 ? m : 0;
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t r = content_rect(app);
    const ic_face_t *f = mono();
    const ic_palette_t *p = ic_palette();
    int cw = cell_w(), ch = cell_h();
    int base = ic_text_center_baseline(f, 0, ch);
    int top_index;

    layout(app);
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    ic_gfx_fill(c, r.x, r.y, r.w, r.h, p->content);
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_CONTROL, 1.0f, p->separator);
    if (term.view_offset > max_view_offset()) term.view_offset = max_view_offset();
    top_index = term.count - term.rows - term.view_offset;

    for (int row = 0; row < term.rows; row++) {
        term_line_t *l = line_at(top_index + row);
        int y = r.y + row * ch;
        int x0 = 0;
        while (x0 < term.cols) {
            term_cell_t *s = &l->cells[x0];
            int inv = (s->attr & ATTR_INVERSE) != 0;
            int x1 = x0 + 1;
            char run[TERM_MAX_COLS + 1];
            int n = 0;
            ic_color_t fc, bc;
            while (x1 < term.cols && l->cells[x1].fg == s->fg && l->cells[x1].bg == s->bg &&
                   l->cells[x1].attr == s->attr) {
                x1++;
            }
            fc = ansi_color(s->fg, 1);
            if ((s->attr & ATTR_BOLD) && s->fg < 8 && s->fg != DEFAULT_FG) fc = ansi_color(s->fg + 8, 1);
            bc = ansi_color(s->bg, 0);
            if (inv) {
                ic_color_t t = fc;
                fc = bc;
                bc = t;
            }
            if (inv || s->bg != DEFAULT_BG) {
                ic_gfx_fill(c, r.x + 5 + x0 * cw, y, (x1 - x0) * cw, ch, bc);
            }
            for (int x = x0; x < x1; x++) run[n++] = l->cells[x].ch;
            while (n > 0 && run[n - 1] == ' ') n--;
            if (n > 0) {
                ic_text_draw_n(c, f, r.x + 5 + x0 * cw, y + base, run, n, fc);
                if (s->attr & ATTR_UNDERLINE) ic_gfx_hline(c, r.x + 5 + x0 * cw, y + ch - 2, n * cw, fc);
            }
            x0 = x1;
        }
    }

    if (term.view_offset == 0 && term.cursor_visible && !term.exited && ic_app_caret_visible(app)) {
        int x = r.x + 5 + term.cx * cw;
        int y = r.y + term.cy * ch;
        if (app->focused) ic_gfx_fill(c, x, y, cw, ch, ic_color_with_alpha(p->label, 0xB0));
        else ic_gfx_rrect_stroke(c, x, y, cw, ch, 1.0f, 1.0f, p->label_secondary);
    }

    {
        int scrolled = term.view_offset > 0;
        float a;
        ic_tween_to(&term.scrollbar, (scrolled || term.hover_scroll || term.dragging_scroll) ? 1.0f : 0.0f,
                    IC_DUR_FAST, scrolled ? IC_EASE_ENTER : IC_EASE_EXIT);
        a = ic_tween_value(&term.scrollbar);
        if (a > 0.01f) {
            ic_ui_scrollbar(c, r, (term.count - term.rows - term.view_offset) * ch, term.count * ch, a);
        }
        if (ic_tween_running(&term.scrollbar)) ic_app_animate(app);
    }
}

static void send_key(const ic_event_t *ev) {
    uint32_t k = ev->key;
    char b[2];
    if (ev->mods & IC_MOD_CTRL) {
        if (k >= 'a' && k <= 'z') {
            b[0] = (char)(k - 'a' + 1);
            pty_send(b, 1);
        }
        return;
    }
    if (ev->mods & IC_MOD_ALT) pty_send("\x1b", 1);
    switch (k) {
    case IC_KEY_ENTER:     pty_send_str("\r"); return;
    case IC_KEY_BACKSPACE: pty_send_str("\b"); return;
    case IC_KEY_TAB:       pty_send_str((ev->mods & IC_MOD_SHIFT) ? "\x1b[Z" : "\t"); return;
    case IC_KEY_ESCAPE:    pty_send_str("\x1b"); return;
    case IC_KEY_UP:        pty_send_str("\x1b[A"); return;
    case IC_KEY_DOWN:      pty_send_str("\x1b[B"); return;
    case IC_KEY_RIGHT:     pty_send_str("\x1b[C"); return;
    case IC_KEY_LEFT:      pty_send_str("\x1b[D"); return;
    case IC_KEY_HOME:      pty_send_str("\x1b[H"); return;
    case IC_KEY_END:       pty_send_str("\x1b[F"); return;
    case IC_KEY_DELETE:    pty_send_str("\x1b[3~"); return;
    case IC_KEY_INSERT:    pty_send_str("\x1b[2~"); return;
    case IC_KEY_PAGE_UP:   pty_send_str("\x1b[5~"); return;
    case IC_KEY_PAGE_DOWN: pty_send_str("\x1b[6~"); return;
    default:
        if (k >= 32 && k < 127) {
            b[0] = (char)k;
            pty_send(b, 1);
        }
        return;
    }
}

static void start_shell(ic_app_t *app) {
    term.exited = 0;
    if (term.pty <= 0) term.pty = icda_pty_open();
    term.pid = 0;
    if (term.pty <= 0) {
        feed_str("terminal: no pseudo-terminal available\r\n");
        term.exited = 1;
        return;
    }
    term.cols = term.rows = 0;
    layout(app);
    term.pid = (uint64_t)icda_pty_spawn(term.pty, TERM_SHELL, 0);
    if ((long)term.pid <= 0) {
        feed_str("terminal: could not start " TERM_SHELL "\r\n");
        term.exited = 1;
    }
}

static void tick(ic_app_t *app) {
    static char buf[4096];
    long n;
    int got = 0;
    if (term.pty <= 0) return;
    while ((n = icda_pty_io(term.pty, ICDA_PTY_READ, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++) feed(buf[i]);
        got = 1;
        if (n < (long)sizeof(buf)) break;
    }
    if (got) {
        term.view_offset = 0;
        ic_app_invalidate(app);
    }
    if (!term.exited && term.pid && icda_ticks() - term.last_alive_check > 25) {
        icda_proc_stats_t st;
        term.last_alive_check = icda_ticks();
        if (icda_proc_stats(term.pid, &st) != 0) {
            feed_str("\r\n\x1b[90m[process exited - press Enter to restart]\x1b[0m\r\n");
            term.exited = 1;
            ic_app_invalidate(app);
        }
    }
}

static void scrollbar_drag(ic_app_t *app, int y) {
    ic_rect_t r = content_rect(app);
    int total = term.count * cell_h();
    int thumb = ic_ui_scrollbar_thumb(r, 0, total).h;
    int usable = r.h - thumb;
    int over = y - r.y - thumb / 2;
    if (usable <= 0) return;
    if (over < 0) over = 0;
    if (over > usable) over = usable;
    term.view_offset = max_view_offset() - (over * max_view_offset() + usable / 2) / usable;
}

static void clamp_view(void) {
    if (term.view_offset < 0) term.view_offset = 0;
    if (term.view_offset > max_view_offset()) term.view_offset = max_view_offset();
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    ic_rect_t r = content_rect(app);
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE: {
        int track_x = r.x + r.w - 10;
        term.hover_scroll = ev->x >= track_x && ic_ui_hit(r, ev->x, ev->y);
        ic_app_set_cursor(app, ic_ui_hit(r, ev->x, ev->y) && !term.hover_scroll ? IC_CURSOR_TEXT
                                                                               : IC_CURSOR_ARROW);
        if (term.dragging_scroll) scrollbar_drag(app, ev->y);
        break;
    }
    case IC_EV_MOUSE_DOWN:
        if (ev->button == GUI_BTN_LEFT && term.hover_scroll) {
            term.dragging_scroll = 1;
            scrollbar_drag(app, ev->y);
        } else if (ev->button == GUI_BTN_RIGHT) {
            char clip[2048];
            long n = ic_clipboard_get(clip, sizeof(clip));
            for (long k = 0; k < n; k++) {
                if (clip[k] == '\n') clip[k] = '\r';
            }
            if (n > 0) pty_send(clip, (int)n);
        }
        break;
    case IC_EV_MOUSE_UP:
        term.dragging_scroll = 0;
        break;
    case IC_EV_MOUSE_LEAVE:
        term.hover_scroll = 0;
        break;
    case IC_EV_SCROLL:
        term.view_offset -= ev->wheel * 3;
        clamp_view();
        break;
    case IC_EV_KEY:
        if ((ev->mods & IC_MOD_SHIFT) && (ev->key == IC_KEY_PAGE_UP || ev->key == IC_KEY_PAGE_DOWN)) {
            term.view_offset += ev->key == IC_KEY_PAGE_UP ? term.rows - 1 : -(term.rows - 1);
            clamp_view();
            break;
        }
        if (term.exited) {
            if (ev->key == IC_KEY_ENTER) start_shell(app);
            break;
        }
        term.view_offset = 0;
        send_key(ev);
        break;
    case IC_EV_RESIZE:
        layout(app);
        break;
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    term.state = PS_NORMAL;
    term.view_offset = 0;
    ic_tween_set(&term.scrollbar, 0.0f);
    start_shell(app);
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Terminal", WIN_W, WIN_H, init, draw, event, tick };
    (void)argc;
    (void)argv;
    if (ic_app_run(&desc, 0) != 0) {
        icda_write("terminal requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
