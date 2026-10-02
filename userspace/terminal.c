














#include "libicda.h"

#define WIN_W 640
#define WIN_H 420

#define TERM_PAD        IC_SP_2
#define TERM_LINE_MAX   512
#define TERM_LINES_MAX  2000
#define TERM_HIST_MAX   64
#define TERM_CMD_MAX    256



typedef enum {
    TERM_ROLE_TEXT = 0,   
    TERM_ROLE_MUTED,      
    TERM_ROLE_ACCENT,     
    TERM_ROLE_ERROR,      
    TERM_ROLE_PROMPT      
} term_role_t;

typedef struct {
    char    text[TERM_LINE_MAX];
    uint8_t role;
} term_line_t;

static struct {
    term_line_t lines[TERM_LINES_MAX];
    int         count;      
    int         head;       

    
    char  cmd[TERM_CMD_MAX];
    int   cmd_len;
    int   cmd_cursor;               
    char  history[TERM_HIST_MAX][TERM_CMD_MAX];
    int   history_count;
    int   history_pos;              
    char  draft[TERM_CMD_MAX];      
    int   draft_saved;

    
    int rows;         
    int cols;         
    int log_rows;     
    int cmd_rows;     
    int total_rows;   
    int cursor_row;   
    int cursor_col;   
    int scroll_rows;  

    int  hover_scroll;
    int  dragging_scroll;

    ic_tween_t scrollbar;
} term;


#define TERM_SCRATCH_PX (192 * 120)
static uint32_t menu_scratch[TERM_SCRATCH_PX];

static const char *const PROMPT = "icda@desktop:~$ ";
#define PROMPT_LEN 14



static term_line_t *log_new(void) {
    if (term.count < TERM_LINES_MAX) return &term.lines[term.count++];
    

    for (int i = 1; i < term.count; i++) term.lines[i - 1] = term.lines[i];
    term.count--;
    term.head++;
    return &term.lines[term.count++];
}

static void log_print(term_role_t role, const char *text) {
    if (!text) return;
    while (*text) {
        int n = 0;
        while (text[n] && text[n] != '\n' && n < TERM_LINE_MAX - 1) n++;
        term_line_t *line = log_new();
        for (int i = 0; i < n; i++) line->text[i] = text[i];
        line->text[n] = '\0';
        line->role = (uint8_t)role;
        text += n;
        if (*text == '\n') text++;
    }
}

static void log_clear(void) {
    term.count = 0;
    term.head = 0;
}



static const ic_face_t *mono(void) { return ic_font(IC_FONT_MONO); }

static int cell_w(void) {
    int w = ic_text_measure(mono(), "0");
    return w > 0 ? w : 8;
}

static ic_rect_t content_rect(ic_app_t *app) {
    return ic_rect_make(TERM_PAD, TERM_PAD, app->width - 2 * TERM_PAD,
                        app->height - 2 * TERM_PAD);
}


static int rows_for(int len, int cols) {
    return len / cols + 1;
}



static void layout(ic_app_t *app) {
    ic_rect_t r = content_rect(app);
    const ic_face_t *f = mono();
    int ch = f->line_h > 0 ? f->line_h : 1;
    int cw = cell_w();
    int rows = r.h / ch;
    int cols = r.w / cw;
    int cpos;

    if (rows < 1) rows = 1;
    if (cols < 8) cols = 8;
    term.rows = rows;
    term.cols = cols;

    term.log_rows = 0;
    for (int i = 0; i < term.count; i++) {
        term.log_rows += rows_for((int)ic_strlen(term.lines[term.head + i].text), cols);
    }
    cpos = PROMPT_LEN + term.cmd_cursor;
    term.cmd_rows = rows_for(PROMPT_LEN + term.cmd_len, cols);
    term.total_rows = term.log_rows + term.cmd_rows;
    term.cursor_row = term.log_rows + cpos / cols;
    term.cursor_col = cpos % cols;

    if (term.scroll_rows > term.total_rows - term.rows) {
        term.scroll_rows = term.total_rows - term.rows;
    }
    if (term.scroll_rows < 0) term.scroll_rows = 0;
}

static int max_scroll(void) {
    int m = term.total_rows - term.rows;
    return m > 0 ? m : 0;
}



static void cmd_set(const char *s) {
    ic_strcpy(term.cmd, s ? s : "", TERM_CMD_MAX);
    term.cmd_len = (int)ic_strlen(term.cmd);
    term.cmd_cursor = term.cmd_len;
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ') s++;
    return s;
}



static int is_cmd(const char *line, const char *word) {
    int n = 0;
    while (word[n]) n++;
    for (int i = 0; i < n; i++) {
        if (line[i] != word[i]) return 0;
    }
    return line[n] == '\0' || line[n] == ' ';
}

static void run_ls(void) {
    char buf[2048];
    uint64_t rc = icda_list_dir(".", buf, sizeof(buf) - 1);
    if (rc == 0 || rc == (uint64_t)-1) {
        log_print(TERM_ROLE_ERROR, "ls: cannot read the current directory");
        return;
    }
    buf[rc] = '\0';
    log_print(TERM_ROLE_TEXT, buf);
}

static void run_cat(const char *arg) {
    char buf[4096];
    uint64_t rc;
    if (!arg || !*arg) {
        log_print(TERM_ROLE_ERROR, "cat: name a file, for example: cat notes.txt");
        return;
    }
    rc = icda_read_file(arg, buf, sizeof(buf) - 1);
    if (rc == (uint64_t)-1) {
        log_print(TERM_ROLE_ERROR, "cat: cannot read that file");
        log_print(TERM_ROLE_MUTED, arg);
        return;
    }
    buf[rc] = '\0';
    log_print(TERM_ROLE_TEXT, buf);
}

static void run_spawn(const char *path) {
    uint64_t pid = icda_spawn(path);
    if (pid == 0 || (int64_t)pid < 0) {
        log_print(TERM_ROLE_ERROR, "Could not launch that program");
    }
}

static void run_help(void) {
    log_print(TERM_ROLE_ACCENT, "Commands");
    log_print(TERM_ROLE_TEXT, "  help              Show this list");
    log_print(TERM_ROLE_TEXT, "  ls                List the current directory");
    log_print(TERM_ROLE_TEXT, "  cat <file>        Print a file");
    log_print(TERM_ROLE_TEXT, "  clear             Clear the scrollback");
    log_print(TERM_ROLE_TEXT, "  run <app>         Launch an app by path");
    log_print(TERM_ROLE_TEXT, "  ps                List running programs");
    log_print(TERM_ROLE_MUTED, "Anything else is launched as a program.");
    log_print(TERM_ROLE_MUTED, "Up and Down walk the history, Page Up and Page Down scroll.");
}

static void run_command(const char *raw) {
    const char *cmd = skip_spaces(raw);
    const char *arg;
    char path[TERM_CMD_MAX];

    if (!*cmd) return;

    if (is_cmd(cmd, "help")) { run_help(); return; }
    if (is_cmd(cmd, "clear")) { log_clear(); return; }
    if (is_cmd(cmd, "ls"))    { run_ls(); return; }
    if (is_cmd(cmd, "ps")) {
        log_print(TERM_ROLE_TEXT, "  PID  PROGRAM");
        log_print(TERM_ROLE_TEXT, "    1  init");
        log_print(TERM_ROLE_MUTED, "Open the Activity window for the full list.");
        return;
    }
    if (is_cmd(cmd, "cat")) {
        run_cat(skip_spaces(cmd + 3));
        return;
    }
    if (is_cmd(cmd, "run")) {
        arg = skip_spaces(cmd + 3);
        if (!*arg) {
            log_print(TERM_ROLE_ERROR, "run: name a program, for example: run /apps/editor.app");
            return;
        }
        log_print(TERM_ROLE_ACCENT, "Launching");
        log_print(TERM_ROLE_TEXT, arg);
        run_spawn(arg);
        return;
    }
    if (ic_strprefix(cmd, "/")) { run_spawn(cmd); return; }
    
    ic_strcpy(path, "/apps/", TERM_CMD_MAX);
    ic_strlcat(path, cmd, TERM_CMD_MAX);
    run_spawn(path);
}

static void history_push(const char *line) {
    if (!line || !line[0]) return;
    if (term.history_count > 0 &&
        ic_strcmp(term.history[term.history_count - 1], line) == 0) {
        return;
    }
    if (term.history_count == TERM_HIST_MAX) {
        for (int i = 1; i < TERM_HIST_MAX; i++) {
            ic_memcpy(term.history[i - 1], term.history[i], TERM_CMD_MAX);
        }
        term.history_count--;
    }
    ic_strcpy(term.history[term.history_count], line, TERM_CMD_MAX);
    term.history_count++;
}

static void submit(void) {
    log_print(TERM_ROLE_PROMPT, PROMPT);
    log_print(TERM_ROLE_TEXT, term.cmd);
    history_push(term.cmd);
    run_command(term.cmd);
    cmd_set("");
    term.history_pos = -1;
    term.draft_saved = 0;
    term.scroll_rows = 0;
}

static void history_step(int dir) {
    if (term.history_count == 0) return;
    if (!term.draft_saved) {
        ic_strcpy(term.draft, term.cmd, TERM_CMD_MAX);
        term.draft_saved = 1;
    }
    if (dir < 0) {
        if (term.history_pos < 0) term.history_pos = term.history_count;
        term.history_pos--;
        if (term.history_pos < 0) term.history_pos = 0;
        cmd_set(term.history[term.history_pos]);
        return;
    }
    if (term.history_pos < 0) return;
    term.history_pos++;
    if (term.history_pos >= term.history_count) {
        term.history_pos = -1;
        term.draft_saved = 0;
        cmd_set(term.draft);
        return;
    }
    cmd_set(term.history[term.history_pos]);
}


static const char *const completions[] = {
    "ls", "cat ", "clear", "help", "ps", "run ",
    "settings.app", "terminal.app", "editor.app", "browser.app",
    "desktop.app", "taskman.app", "diskman.app", "audioplay.app"
};

static void complete(void) {
    int tl = (int)ic_strlen(term.cmd);
    for (unsigned i = 0; i < sizeof(completions) / sizeof(completions[0]); i++) {
        const char *cand = completions[i];
        int n = 0;
        while (cand[n]) n++;
        int match = tl <= n;
        for (int k = 0; match && k < tl; k++) {
            if (ic_lower(term.cmd[k]) != ic_lower(cand[k])) match = 0;
        }
        if (match) {
            ic_strcpy(term.cmd, cand, TERM_CMD_MAX);
            term.cmd_len = (int)ic_strlen(term.cmd);
            term.cmd_cursor = term.cmd_len;
            term.history_pos = -1;
            return;
        }
    }
}


static int prev_offset(const char *s, int at) {
    int i = at - 1;
    if (i <= 0) return 0;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

static int next_offset(const char *s, int at, int len) {
    int i = at + 1;
    if (at >= len) return len;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static void edit_key(uint32_t key) {
    switch (key) {
    case IC_KEY_LEFT:
        term.cmd_cursor = prev_offset(term.cmd, term.cmd_cursor);
        break;
    case IC_KEY_RIGHT:
        term.cmd_cursor = next_offset(term.cmd, term.cmd_cursor, term.cmd_len);
        break;
    case IC_KEY_HOME:
        term.cmd_cursor = 0;
        break;
    case IC_KEY_END:
        term.cmd_cursor = term.cmd_len;
        break;
    case IC_KEY_BACKSPACE:
        if (term.cmd_cursor > 0) {
            int start = prev_offset(term.cmd, term.cmd_cursor);
            for (int i = term.cmd_cursor; i < term.cmd_len; i++) {
                term.cmd[i - 1] = term.cmd[i];
            }
            term.cmd_len--;
            term.cmd[term.cmd_len] = '\0';
            term.cmd_cursor = start;
        }
        break;
    case IC_KEY_DELETE:
        if (term.cmd_cursor < term.cmd_len) {
            int end = next_offset(term.cmd, term.cmd_cursor, term.cmd_len);
            for (int i = end; i < term.cmd_len; i++) term.cmd[i - 1] = term.cmd[i];
            term.cmd_len--;
            term.cmd[term.cmd_len] = '\0';
        }
        break;
    default:
        return;
    }
    term.history_pos = -1;
}

static void type_char(uint32_t key) {
    if (term.cmd_len + 1 >= TERM_CMD_MAX) return;
    for (int i = term.cmd_len; i > term.cmd_cursor; i--) term.cmd[i] = term.cmd[i - 1];
    term.cmd[term.cmd_cursor++] = (char)key;
    term.cmd_len++;
    term.cmd[term.cmd_len] = '\0';
    term.history_pos = -1;
}



enum { TR_CLEAR = 1, TR_NEW, TR_QUIT };

static ic_menu_model_t tr_menu;
static int tr_menu_x, tr_menu_y;
static int tr_menu_open;

static void tr_menu_build(void) {
    for (int i = 0; i < 3; i++) {
        tr_menu.shortcuts[i] = 0;
        tr_menu.disabled[i] = 0;
    }
    tr_menu.labels[0] = "Clear Buffer";
    tr_menu.labels[1] = "New Terminal";
    tr_menu.labels[2] = "Quit";
    tr_menu.count = 3;
    tr_menu.hover = -1;
}

static void tr_menu_open_at(ic_app_t *app, int x, int y) {
    int mw, mh;
    tr_menu_build();
    mw = ic_ui_menu_width(&tr_menu);
    mh = ic_ui_menu_height(&tr_menu);
    if (x + mw > app->width - IC_SP_2) x = app->width - IC_SP_2 - mw;
    if (y + mh > app->height - IC_SP_2) y = app->height - IC_SP_2 - mh;
    if (x < IC_SP_2) x = IC_SP_2;
    if (y < IC_SP_2) y = IC_SP_2;
    tr_menu_x = x;
    tr_menu_y = y;
    tr_menu_open = 1;
}

static void tr_menu_activate(ic_app_t *app, int which) {
    tr_menu_open = 0;
    switch (which) {
    case TR_CLEAR: log_clear(); break;
    case TR_NEW:  icda_spawn("/apps/terminal.app"); break;
    case TR_QUIT: ic_app_quit(app); break;
    default: break;
    }
}



static ic_color_t role_color(uint8_t role) {
    const ic_palette_t *p = ic_palette();
    switch (role) {
    case TERM_ROLE_MUTED:  return p->label_tertiary;
    case TERM_ROLE_ACCENT: return p->accent;
    case TERM_ROLE_ERROR:  return p->danger;
    case TERM_ROLE_PROMPT: return p->success;
    default:               return p->label;
    }
}



static int compose_command(char *out, int cap) {
    int n = 0;
    for (int i = 0; i < PROMPT_LEN && n < cap - 1; i++) out[n++] = PROMPT[i];
    for (int i = 0; i < term.cmd_len && n < cap - 1; i++) out[n++] = term.cmd[i];
    out[n] = '\0';
    return n;
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t r = content_rect(app);
    const ic_face_t *f = mono();
    const ic_palette_t *p = ic_palette();
    int ch = f->line_h;
    int baseline0;
    int first, last, row, i;
    char cmdline[PROMPT_LEN + TERM_CMD_MAX + 2];
    int cmdlen;
    int scrolled;

    layout(app);

    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    ic_gfx_fill(c, r.x, r.y, r.w, r.h, p->content);
    
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_CONTROL, 1.0f, p->separator);

    baseline0 = r.y + ic_text_center_baseline(f, 0, ch);
    first = term.total_rows - term.rows - term.scroll_rows;
    if (first < 0) first = 0;
    last = first + term.rows;
    if (last > term.total_rows) last = term.total_rows;

    
    row = 0;
    for (i = 0; i < term.count && row < last; i++) {
        const term_line_t *line = &term.lines[term.head + i];
        int len = (int)ic_strlen(line->text);
        int col = 0;
        





        int owned = rows_for(len, term.cols);
        int drawn = 0;
        while (col < len) {
            int n = len - col;
            if (n > term.cols) n = term.cols;
            if (row >= first && row < last) {
                ic_rect_t saved;
                ic_canvas_push_clip(c, r.x, r.y + row * ch, r.w, ch, &saved);
                ic_text_draw_n(c, f, r.x, baseline0 + row * ch, line->text + col, n,
                               role_color(line->role));
                ic_canvas_pop_clip(c, &saved);
            }
            col += n;
            row++;
            drawn++;
        }
        row += owned - drawn;
    }

    
    cmdlen = compose_command(cmdline, (int)sizeof(cmdline));
    {
        int col = 0;
        int crow = term.log_rows;
        while (col < cmdlen && crow < last) {
            int n = cmdlen - col;
            int pcol = col;
            if (n > term.cols) n = term.cols;
            if (crow >= first) {
                ic_rect_t saved;
                ic_canvas_push_clip(c, r.x, r.y + crow * ch, r.w, ch, &saved);
                if (pcol < PROMPT_LEN) {
                    int pn = PROMPT_LEN - pcol;
                    if (pn > n) pn = n;
                    ic_text_draw_n(c, f, r.x, baseline0 + crow * ch, PROMPT + pcol, pn,
                                   p->success);
                    if (pn < n) {
                        ic_text_draw_n(c, f, r.x + ic_text_measure_n(f, PROMPT + pcol, pn),
                                       baseline0 + crow * ch, cmdline + PROMPT_LEN, n - pn,
                                       p->label);
                    }
                } else {
                    ic_text_draw_n(c, f, r.x, baseline0 + crow * ch, cmdline + pcol, n,
                                   p->label);
                }
                ic_canvas_pop_clip(c, &saved);
            }
            col += n;
            crow++;
        }
    }

    
    if (app->focused && term.cursor_row >= first && term.cursor_row < last &&
        ic_app_caret_visible(app)) {
        int cx = r.x + term.cursor_col * cell_w();
        int cy = r.y + (term.cursor_row - first) * ch;
        if (cx + 3 <= r.x + r.w) ic_gfx_fill(c, cx, cy, 2, ch, p->label);
    }

    
    scrolled = term.scroll_rows > 0;
    ic_tween_to(&term.scrollbar,
                (scrolled || term.hover_scroll || term.dragging_scroll) ? 1.0f : 0.0f,
                IC_DUR_FAST, scrolled ? IC_EASE_ENTER : IC_EASE_EXIT);
    {
        float a = ic_tween_value(&term.scrollbar);
        if (a > 0.01f) ic_ui_scrollbar(c, r, first, term.total_rows, a);
    }
    if (ic_tween_running(&term.scrollbar)) ic_app_animate(app);

    

    if (scrolled) {
        char note[64];
        int w;
        int nx, ny;
        ic_strcpy(note, "Scrolled back - press End to follow", sizeof(note));
        w = ic_text_measure(ic_font(IC_FONT_FOOTNOTE), note) + IC_SP_4;
        nx = r.x + r.w - w - IC_SP_2;
        ny = r.y + IC_SP_2;
        if (nx < r.x + IC_SP_2) nx = r.x + IC_SP_2;
        ic_ui_panel(c, ic_rect_make(nx, ny, w, 22), IC_R_MENU, IC_ELEV_MENU,
                    menu_scratch, TERM_SCRATCH_PX);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(nx, ny, w, 22), note,
                        p->label_secondary, IC_ALIGN_CENTER);
        ic_app_animate(app);
    }

    if (tr_menu_open) {
        tr_menu.hover = ic_ui_menu_hit(&tr_menu, tr_menu_x, tr_menu_y,
                                      app->mouse_x, app->mouse_y);
        ic_ui_menu(c, &tr_menu, tr_menu_x, tr_menu_y, menu_scratch, TERM_SCRATCH_PX);
    }
    
    if (app->focused) ic_app_animate(app);
}



static void scroll_by(int rows) {
    int max = max_scroll();
    term.scroll_rows += rows;
    if (term.scroll_rows > max) term.scroll_rows = max;
    if (term.scroll_rows < 0) term.scroll_rows = 0;
}



static void scrollbar_drag(ic_app_t *app, int y) {
    ic_rect_t r = content_rect(app);
    int track_h = r.h;
    int thumb = ic_ui_scrollbar_thumb(r, 0, term.total_rows).h;
    int usable = track_h - thumb;
    int over;
    if (usable <= 0) return;
    over = y - r.y - thumb / 2;
    if (over < 0) over = 0;
    if (over > usable) over = usable;
    term.scroll_rows = (over * max_scroll() + usable / 2) / usable;
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    ic_rect_t r = content_rect(app);

    switch (ev->type) {
    case IC_EV_MOUSE_MOVE: {
        int track_x = r.x + r.w - 6;
        term.hover_scroll = ev->x >= track_x - 2 && ic_ui_hit(r, ev->x, ev->y);
        if (term.dragging_scroll) scrollbar_drag(app, ev->y);
        break;
    }
    case IC_EV_MOUSE_DOWN:
        if (ev->button == GUI_BTN_RIGHT) {
            if (tr_menu_open) {
                int which = ic_ui_menu_hit(&tr_menu, tr_menu_x, tr_menu_y, ev->x, ev->y);
                tr_menu_open = 0;
                if (which >= 0) tr_menu_activate(app, which);
            } else {
                tr_menu_open_at(app, ev->x, ev->y);
            }
            break;
        }
        if (ev->button != GUI_BTN_LEFT) break;
        if (tr_menu_open) {
            int which = ic_ui_menu_hit(&tr_menu, tr_menu_x, tr_menu_y, ev->x, ev->y);
            tr_menu_open = 0;
            if (which >= 0) tr_menu_activate(app, which);
            break;
        }
        if (term.hover_scroll) term.dragging_scroll = 1;
        break;
    case IC_EV_MOUSE_UP:
        if (ev->button == GUI_BTN_LEFT) term.dragging_scroll = 0;
        break;
    case IC_EV_MOUSE_LEAVE:
        term.hover_scroll = 0;
        term.dragging_scroll = 0;
        break;
    case IC_EV_KEY:
        if (tr_menu_open) { tr_menu_open = 0; break; }
        switch (ev->key) {
        case IC_KEY_PAGE_UP:   scroll_by(-term.rows); break;
        case IC_KEY_PAGE_DOWN: scroll_by(term.rows); break;
        case IC_KEY_UP:
            if (term.scroll_rows > 0) scroll_by(-1);
            else history_step(-1);
            break;
        case IC_KEY_DOWN:
            if (term.scroll_rows > 0) scroll_by(1);
            else history_step(1);
            break;
        case IC_KEY_END:
            if (term.scroll_rows > 0) term.scroll_rows = 0;
            else { term.cmd_cursor = term.cmd_len; }
            break;
        case IC_KEY_HOME:     term.cmd_cursor = 0; break;
        case IC_KEY_ENTER:    submit(); break;
        case IC_KEY_TAB:      complete(); break;
        case IC_KEY_ESCAPE:
            if (term.scroll_rows > 0) {
                term.scroll_rows = 0;
            } else {
                cmd_set("");
                term.draft_saved = 0;
                term.history_pos = -1;
            }
            break;
        case IC_KEY_BACKSPACE:
        case IC_KEY_DELETE:
        case IC_KEY_LEFT:
        case IC_KEY_RIGHT:
            edit_key(ev->key);
            break;
        default:
            if (ev->key >= 32 && ev->key < 127) type_char(ev->key);
            break;
        }
        break;
    case IC_EV_RESIZE:
        

        layout(app);
        break;
    case IC_EV_APPEARANCE:
    case IC_EV_FOCUS:
    case IC_EV_BLUR:
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    cmd_set("");
    term.history_pos = -1;
    term.scroll_rows = 0;
    term.hover_scroll = 0;
    term.dragging_scroll = 0;
    term.draft_saved = 0;
    term.history_count = 0;
    tr_menu_open = 0;
    ic_tween_set(&term.scrollbar, 0.0f);

    log_print(TERM_ROLE_ACCENT, "ICDA Terminal");
    log_print(TERM_ROLE_MUTED, "Type help for the command list.");
    layout(app);
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Terminal", WIN_W, WIN_H, init, draw, event, 0 };
    (void)argc;
    (void)argv;
    if (ic_app_run(&desc, 0) != 0) {
        icda_write("terminal requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
