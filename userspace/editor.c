












#include "libicda.h"

#define WIN_W 700
#define WIN_H 480

#define EDIT_BUF_CAP    8192
#define EDIT_PATH_CAP   128
#define EDIT_STATUS_CAP 96
#define EDIT_GUTTER_W   46
#define EDIT_TAB        4

static struct {
    char     path[EDIT_PATH_CAP];
    char     buf[EDIT_BUF_CAP];
    uint64_t len;
    uint64_t cursor;          
    uint64_t want_col;        
    int      modified;
    int      scroll_row;
    int      scroll_col;
    uint64_t followed;

    
    int rows;
    int cols;
    int text_x, text_y, text_w, text_h;
    int ch, cw;

    
    int hover_save;
    int hover_new;
    int confirm_new;

    char status[EDIT_STATUS_CAP];
} ed;



static ic_rect_t toolbar_rect(ic_app_t *app) {
    return ic_rect_make(0, 0, app->width, IC_H_TOOLBAR);
}

static ic_rect_t text_rect(ic_app_t *app) {
    int y = IC_H_TOOLBAR;
    return ic_rect_make(0, y, app->width, app->height - y - 24);
}

static ic_rect_t new_rect(ic_app_t *app) {
    ic_rect_t b = toolbar_rect(app);
    int w = ic_ui_button_width("New", IC_SYM_NONE);
    return ic_rect_make(b.x + IC_SP_4, (b.h - IC_H_CONTROL_SM) / 2, w, IC_H_CONTROL_SM);
}

static ic_rect_t save_rect(ic_app_t *app) {
    ic_rect_t r = new_rect(app);
    int w = ic_ui_button_width("Save", IC_SYM_NONE);
    return ic_rect_make(r.x + r.w + IC_SP_2, r.y, w, IC_H_CONTROL_SM);
}

static const ic_face_t *mono(void) { return ic_font(IC_FONT_MONO); }


static void layout(ic_app_t *app) {
    ic_rect_t t = text_rect(app);
    const ic_face_t *f = mono();
    ed.ch = f->line_h > 0 ? f->line_h : 1;
    ed.cw = ic_text_measure(f, "0");
    if (ed.cw <= 0) ed.cw = 8;
    ed.text_x = EDIT_GUTTER_W + IC_SP_2;
    ed.text_y = t.y + IC_SP_2;
    ed.text_w = t.w - ed.text_x - IC_SP_3;
    ed.text_h = t.h - 2 * IC_SP_2;
    ed.rows = ed.text_h / ed.ch;
    ed.cols = ed.text_w / ed.cw;
    if (ed.rows < 1) ed.rows = 1;
    if (ed.cols < 8) ed.cols = 8;
}



static uint64_t line_start(uint64_t pos) {
    if (pos > ed.len) pos = ed.len;
    while (pos > 0 && ed.buf[pos - 1] != '\n') pos--;
    return pos;
}

static uint64_t line_end(uint64_t pos) {
    if (pos > ed.len) pos = ed.len;
    while (pos < ed.len && ed.buf[pos] != '\n') pos++;
    return pos;
}

static uint64_t row_of(uint64_t pos) {
    uint64_t row = 0;
    for (uint64_t i = 0; i < pos && i < ed.len; i++) {
        if (ed.buf[i] == '\n') row++;
    }
    return row;
}

static int visual_col(uint64_t pos) {
    int c = 0;
    for (uint64_t i = line_start(pos); i < pos; i++) {
        if (ed.buf[i] == '\t') c += EDIT_TAB - (c % EDIT_TAB);
        else c++;
    }
    return c;
}

static uint64_t offset_of_row(uint64_t row) {
    uint64_t r = 0;
    uint64_t pos = 0;
    while (pos < ed.len && r < row) {
        if (ed.buf[pos++] == '\n') r++;
    }
    return pos;
}

static void ed_status(const char *text) {
    ic_strcpy(ed.status, text, EDIT_STATUS_CAP);
}


static void scroll_to_cursor(void) {
    if (ed.cursor == ed.followed) return;
    ed.followed = ed.cursor;
    uint64_t row = row_of(ed.cursor);
    int col = visual_col(ed.cursor);
    if ((int)row < ed.scroll_row) ed.scroll_row = (int)row;
    if ((int)row >= ed.scroll_row + ed.rows) ed.scroll_row = (int)row - ed.rows + 1;
    if (col < ed.scroll_col) ed.scroll_col = col;
    if (col >= ed.scroll_col + ed.cols) ed.scroll_col = col - ed.cols + 1;
    if (ed.scroll_row < 0) ed.scroll_row = 0;
    if (ed.scroll_col < 0) ed.scroll_col = 0;
}



static void insert_char(char ch) {
    if (ed.len + 1 >= EDIT_BUF_CAP) {
        ed_status("Document is full");
        return;
    }
    for (uint64_t i = ed.len; i > ed.cursor; i--) ed.buf[i] = ed.buf[i - 1];
    ed.buf[ed.cursor] = ch;
    ed.len++;
    ed.cursor++;
    ed.buf[ed.len] = 0;
    ed.modified = 1;
}

static void backspace(void) {
    if (ed.cursor == 0) return;
    for (uint64_t i = ed.cursor - 1; i < ed.len; i++) ed.buf[i] = ed.buf[i + 1];
    ed.cursor--;
    ed.len--;
    ed.buf[ed.len] = 0;
    ed.modified = 1;
}

static void delete_forward(void) {
    if (ed.cursor >= ed.len) return;
    for (uint64_t i = ed.cursor; i < ed.len; i++) ed.buf[i] = ed.buf[i + 1];
    ed.len--;
    ed.buf[ed.len] = 0;
    ed.modified = 1;
}

static void move_vertical(int direction) {
    uint64_t start = line_start(ed.cursor);
    uint64_t col = ed.cursor - start;
    uint64_t target;
    uint64_t end;

    if (direction < 0) {
        if (start == 0) return;
        target = line_start(start - 1);
        end = line_end(target);
    } else {
        target = line_end(start);
        if (target >= ed.len) return;
        target++;
        end = line_end(target);
    }
    ed.cursor = target + col;
    if (ed.cursor > end) ed.cursor = end;
}

static void move_to(uint64_t pos) {
    if (pos > ed.len) pos = ed.len;
    ed.cursor = pos;
}

static void open_file(const char *path) {
    long n;
    ic_strcpy(ed.path, path, EDIT_PATH_CAP);
    n = (long)icda_read_file(ed.path, ed.buf, sizeof(ed.buf) - 1);
    if (n < 0) {
        ed.buf[0] = 0;
        ed.len = 0;
        ed_status("New document");
    } else {
        ed.len = (uint64_t)n;
        ed.buf[ed.len] = 0;
        ed_status("Opened");
    }
    ed.cursor = 0;
    ed.scroll_row = 0;
    ed.scroll_col = 0;
    ed.modified = 0;
}

static void save(void) {
    
    if (icda_write_file(ed.path, ed.buf, ed.len) != (uint64_t)-1) {
        ed.modified = 0;
        ed_status("Saved");
    } else {
        ed_status("Could not save this file");
    }
}



static void draw_toolbar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t b = toolbar_rect(app);
    ic_rect_t s = save_rect(app);
    const ic_palette_t *p = ic_palette();

    ic_ui_toolbar(c, b);
    ic_ui_button(c, new_rect(app), "New", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 ed.hover_new ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_button(c, s, "Save", IC_SYM_NONE,
                 ed.modified ? IC_BUTTON_PRIMARY : IC_BUTTON_DEFAULT,
                 ed.hover_save ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY),
                    ic_rect_make(s.x + s.w + IC_SP_3, 0, b.w - (s.x + s.w) - IC_SP_3, b.h),
                    ed.path, p->label_secondary, IC_ALIGN_LEFT);
}

static void draw_text(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t t = text_rect(app);
    const ic_palette_t *p = ic_palette();
    const ic_face_t *f = mono();
    const ic_face_t *num = ic_font(IC_FONT_MONO_SMALL);
    ic_rect_t saved;
    uint64_t cursor_row = row_of(ed.cursor);
    uint64_t last_row = row_of(ed.len);
    char digits[24];

    ic_gfx_fill(c, t.x, t.y, t.w, t.h, p->content);

    ic_canvas_push_clip(c, t.x, t.y, t.w, t.h, &saved);

    
    ic_gfx_fill(c, t.x, t.y, EDIT_GUTTER_W, t.h, p->sidebar);
    for (int r = 0; r < ed.rows; r++) {
        uint64_t row = (uint64_t)(ed.scroll_row + r);
        ic_rect_t rr = ic_rect_make(t.x, ed.text_y + r * ed.ch, EDIT_GUTTER_W, ed.ch);
        int n;
        if (row > last_row) break;
        if (row == cursor_row) ic_gfx_fill(c, rr.x, rr.y, rr.w, rr.h, p->accent_soft);
        n = (int)ic_snprintf_u64(digits, sizeof(digits), row + 1);
        ic_text_draw_n(c, num,
                       t.x + EDIT_GUTTER_W - IC_SP_2 -
                           ic_text_measure_n(num, digits, n),
                       ed.text_y + r * ed.ch + ic_text_center_baseline(f, 0, ed.ch),
                       digits, n, row == cursor_row ? p->accent : p->label_tertiary);
    }
    ic_gfx_vline(c, EDIT_GUTTER_W, t.y, t.h, p->separator);

    


    for (int r = 0; r < ed.rows; r++) {
        uint64_t row = (uint64_t)(ed.scroll_row + r);
        uint64_t start = offset_of_row(row);
        uint64_t end;
        int y = ed.text_y + r * ed.ch + ic_text_center_baseline(f, 0, ed.ch);
        int col = 0;
        uint64_t pos = start;
        if (row > last_row) break;
        end = line_end(start);
        while (pos < end && col < ed.scroll_col + ed.cols) {
            uint64_t run_end = pos;
            int run_start_col = col;
            unsigned char first = (unsigned char)ed.buf[pos];
            if (first == '\t') {
                col += EDIT_TAB - (col % EDIT_TAB);
                pos++;
                continue;
            }
            if (first < 32 || first > 126) {
                if (col >= ed.scroll_col) {
                    ic_text_draw_n(c, f, ed.text_x + (col - ed.scroll_col) * ed.cw, y, "?", 1,
                                   p->label_tertiary);
                }
                col++;
                pos++;
                continue;
            }
            while (run_end < end) {
                unsigned char ch = (unsigned char)ed.buf[run_end];
                if (ch == '\t' || ch < 32 || ch > 126) break;
                run_end++;
            }
            {
                int end_col = run_start_col + (int)(run_end - pos);
                int vis_from = run_start_col > ed.scroll_col ? run_start_col : ed.scroll_col;
                int vis_to = end_col < ed.scroll_col + ed.cols ? end_col : ed.scroll_col + ed.cols;
                if (vis_from < vis_to) {
                    ic_text_draw_n(c, f, ed.text_x + (vis_from - ed.scroll_col) * ed.cw, y,
                                   ed.buf + pos + (vis_from - run_start_col), vis_to - vis_from,
                                   p->label);
                }
                col = end_col;
                pos = run_end;
            }
        }
    }
    ic_canvas_pop_clip(c, &saved);
}

static void draw_caret(ic_app_t *app, ic_canvas_t *c) {
    uint64_t row = row_of(ed.cursor);
    int col = visual_col(ed.cursor);
    int r = (int)row - ed.scroll_row;
    int cc = col - ed.scroll_col;
    if (r < 0 || r >= ed.rows || cc < 0 || cc >= ed.cols) return;
    if (!ic_app_caret_visible(app)) return;
    ic_gfx_fill(c, ed.text_x + cc * ed.cw, ed.text_y + r * ed.ch, 2, ed.ch,
                ic_palette()->label);
}

static void draw_status(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = ic_rect_make(0, app->height - 24, app->width, 24);
    const ic_palette_t *p = ic_palette();
    char pos[48];
    char right[64];
    char n[24];

    pos[0] = 0;
    ic_strlcat(pos, "Ln ", sizeof(pos));
    ic_snprintf_u64(n, sizeof(n), row_of(ed.cursor) + 1);
    ic_strlcat(pos, n, sizeof(pos));
    ic_strlcat(pos, ", Col ", sizeof(pos));
    ic_snprintf_u64(n, sizeof(n), (uint64_t)visual_col(ed.cursor) + 1);
    ic_strlcat(pos, n, sizeof(pos));

    right[0] = 0;
    ic_snprintf_u64(n, sizeof(n), ed.len);
    ic_strlcat(right, n, sizeof(right));
    ic_strlcat(right, " bytes", sizeof(right));

    ic_ui_statusbar(c, s, ed.status);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                    ic_rect_make(s.x + s.w - 260, s.y, 110, s.h), pos, p->label_secondary,
                    IC_ALIGN_RIGHT);
    ic_text_draw_in(c, ic_font(IC_FONT_MONO_SMALL),
                    ic_rect_make(s.x + s.w - 140, s.y, 140 - IC_SP_3, s.h), right,
                    p->label_secondary, IC_ALIGN_RIGHT);
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    layout(app);
    scroll_to_cursor();
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_toolbar(app, c);
    draw_text(app, c);
    draw_caret(app, c);
    draw_status(app, c);
    if (app->focused) ic_app_animate(app);
}




static void click_to_cursor(int mx, int my) {
    int r = (my - ed.text_y) / ed.ch;
    int col = (mx - ed.text_x) / ed.cw + ed.scroll_col;
    uint64_t row = (uint64_t)(ed.scroll_row + (r < 0 ? 0 : r));
    uint64_t start;
    uint64_t end;
    uint64_t pos;
    int c = 0;
    if (r < 0 || r >= ed.rows) return;
    start = offset_of_row(row);
    end = line_end(start);
    pos = start;
    while (pos < end && c < col) {
        if (ed.buf[pos] == '\t') c += EDIT_TAB - (c % EDIT_TAB);
        else c++;
        pos++;
    }
    move_to(pos);
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        ed.hover_new = ic_ui_hit(new_rect(app), ev->x, ev->y);
        ed.hover_save = ic_ui_hit(save_rect(app), ev->x, ev->y);
        break;
    case IC_EV_MOUSE_DOWN:
        if (ev->button != GUI_BTN_LEFT) break;
        if (ed.hover_new) {
            if (ed.modified && !ed.confirm_new) {
                ed.confirm_new = 1;
                ed_status("Unsaved changes. Click New again to discard them.");
                break;
            }
            ed.confirm_new = 0;
            ic_strcpy(ed.path, "/home/untitled.txt", EDIT_PATH_CAP);
            ed.buf[0] = 0;
            ed.len = 0;
            ed.cursor = 0;
            ed.modified = 0;
            ed.scroll_row = 0;
            ed.scroll_col = 0;
            ed_status("New document");
            break;
        }
        ed.confirm_new = 0;
        if (ed.hover_save) { save(); break; }
        click_to_cursor(ev->x, ev->y);
        break;
    case IC_EV_MOUSE_LEAVE:
        ed.hover_new = ed.hover_save = 0;
        break;
    case IC_EV_SCROLL: {
        int max_row = (int)row_of(ed.len);
        ed.scroll_row += ev->wheel * 3;
        if (ed.scroll_row > max_row) ed.scroll_row = max_row;
        if (ed.scroll_row < 0) ed.scroll_row = 0;
        break;
    }
    case IC_EV_KEY:
        ed.confirm_new = 0;
        switch (ev->key) {
        case IC_KEY_LEFT:      move_to(ed.cursor > 0 ? ed.cursor - 1 : 0); break;
        case IC_KEY_RIGHT:     move_to(ed.cursor + 1); break;
        case IC_KEY_UP:        move_vertical(-1); break;
        case IC_KEY_DOWN:      move_vertical(1); break;
        case IC_KEY_HOME:      move_to(line_start(ed.cursor)); break;
        case IC_KEY_END:       move_to(line_end(ed.cursor)); break;
        case IC_KEY_PAGE_UP:
            for (int i = 0; i < ed.rows; i++) move_vertical(-1);
            break;
        case IC_KEY_PAGE_DOWN:
            for (int i = 0; i < ed.rows; i++) move_vertical(1);
            break;
        case IC_KEY_BACKSPACE: backspace(); break;
        case IC_KEY_DELETE:    delete_forward(); break;
        case IC_KEY_ENTER:     insert_char('\n'); break;
        case IC_KEY_TAB:       insert_char('\t'); break;
        default:
            if (ev->key >= 32 && ev->key < 127) insert_char((char)ev->key);
            break;
        }
        break;
    case IC_EV_RESIZE:
        layout(app);
        break;
    case IC_EV_FOCUS:
    case IC_EV_APPEARANCE:
    case IC_EV_BLUR:
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    long n = (long)icda_read_file("/home/.edit.request", ed.path, sizeof(ed.path));
    if (app->user) {
        
        const char *arg = (const char *)app->user;
        if (arg[0]) {
            open_file(arg);
            ed.cursor = 0;
            ed.scroll_row = 0;
            ed.scroll_col = 0;
            layout(app);
            return;
        }
    }
    if (n <= 0) {
        ic_strcpy(ed.path, "/home/untitled.txt", EDIT_PATH_CAP);
        ed.buf[0] = 0;
        ed.len = 0;
        ed_status("New document");
    } else {
        if ((uint64_t)n >= sizeof(ed.path)) n = (long)sizeof(ed.path) - 1;
        ed.path[n] = 0;
        open_file(ed.path);
    }
    ed.cursor = 0;
    ed.modified = 0;
    ed.scroll_row = 0;
    ed.scroll_col = 0;
    layout(app);
    ed_status("Ready");
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Editor", WIN_W, WIN_H, init, draw, event, 0 };
    const char *arg = (argc > 1 && argv) ? argv[1] : 0;
    if (ic_app_run(&desc, (void *)arg) != 0) {
        icda_write("editor requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
