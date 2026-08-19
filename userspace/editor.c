/*
 * editor.app - ICDA Text Editor (GUI).
 *
 * A real desktop window (previously a console program that only ran on
 * the text virtual terminal).  Opens the file from /home/.edit.request
 * (falling back to /home/untitled.txt), edits it in a window with the
 * same core as the old console editor, and saves with Ctrl+S.
 */
#include "gui.h"
#include "icda_sys.h"
#include "libicda.h"
#include "font.h"

#include <stdint.h>

#define EDIT_REQUEST_PATH "/home/.edit.request"
#define EDIT_DEFAULT_PATH "/home/untitled.txt"
#define EDIT_PATH_CAP 128
#define EDIT_BUF_CAP 4096

/* Window-relative text area */
#define ED_TOP_H 64
#define ED_STATUS_H 30
#define ED_GUTTER_W 34

enum {
    KEY_SPECIAL_BASE = 256,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_DELETE
};

static char editor_path[EDIT_PATH_CAP];
static char editor_buf[EDIT_BUF_CAP];
static uint64_t editor_len = 0;
static uint64_t editor_cursor = 0;
static int editor_modified = 0;
static uint64_t editor_top_row = 0;
static char status_msg[64] = "Ctrl+S save   Ctrl+X exit   arrows move";

static uint64_t ed_strlen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void ed_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void ed_append_uint(char *dst, uint64_t value, uint64_t cap) {
    char num[32];
    uint64_t i = sizeof(num) - 1;
    uint64_t at = ed_strlen(dst);
    num[i] = 0;
    if (value == 0) {
        num[--i] = '0';
    } else {
        while (value && i > 0) {
            num[--i] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    while (num[i] && at + 1 < cap) {
        dst[at++] = num[i++];
    }
    dst[at] = 0;
}

/* ---- core editing (unchanged from the console editor) ---------------- */

static uint64_t editor_line_start(const char *buf, uint64_t len, uint64_t pos) {
    if (pos > len) pos = len;
    while (pos > 0 && buf[pos - 1] != '\n') pos--;
    return pos;
}

static uint64_t editor_line_end(const char *buf, uint64_t len, uint64_t pos) {
    if (pos > len) pos = len;
    while (pos < len && buf[pos] != '\n') pos++;
    return pos;
}

static uint64_t editor_column(const char *buf, uint64_t len, uint64_t pos) {
    return pos - editor_line_start(buf, len, pos);
}

static uint64_t editor_line_number(const char *buf, uint64_t pos) {
    uint64_t line = 1;
    for (uint64_t i = 0; i < pos && buf[i]; i++) {
        if (buf[i] == '\n') line++;
    }
    return line;
}

static uint64_t editor_find_row_start(const char *buf, uint64_t len, uint64_t target_row) {
    uint64_t row = 0;
    uint64_t pos = 0;
    while (pos < len && row < target_row) {
        if (buf[pos++] == '\n') row++;
    }
    return pos;
}

static uint64_t editor_cursor_row(const char *buf, uint64_t pos) {
    uint64_t row = 0;
    for (uint64_t i = 0; i < pos && buf[i]; i++) {
        if (buf[i] == '\n') row++;
    }
    return row;
}

static void editor_insert_char(char *buf, uint64_t *len, uint64_t *cursor, char ch, uint64_t cap) {
    if (!buf || !len || !cursor || *len + 1 >= cap) return;
    for (uint64_t i = *len; i > *cursor; i--) buf[i] = buf[i - 1];
    buf[*cursor] = ch;
    (*len)++;
    (*cursor)++;
    buf[*len] = 0;
}

static void editor_backspace(char *buf, uint64_t *len, uint64_t *cursor) {
    if (!buf || !len || !cursor || *cursor == 0) return;
    for (uint64_t i = *cursor - 1; i < *len; i++) buf[i] = buf[i + 1];
    (*cursor)--;
    (*len)--;
    buf[*len] = 0;
}

static void editor_delete(char *buf, uint64_t *len, uint64_t *cursor) {
    if (!buf || !len || !cursor || *cursor >= *len) return;
    for (uint64_t i = *cursor; i < *len; i++) buf[i] = buf[i + 1];
    (*len)--;
    buf[*len] = 0;
}

static void editor_move_left(uint64_t *cursor) {
    if (*cursor > 0) (*cursor)--;
}

static void editor_move_right(uint64_t len, uint64_t *cursor) {
    if (*cursor < len) (*cursor)++;
}

static void editor_move_vertical(const char *buf, uint64_t len, uint64_t *cursor, int direction) {
    uint64_t current_start = editor_line_start(buf, len, *cursor);
    uint64_t current_col = *cursor - current_start;
    uint64_t target_start;
    uint64_t target_end;

    if (direction < 0) {
        if (current_start == 0) return;
        target_end = current_start - 1;
        target_start = editor_line_start(buf, len, target_end);
    } else {
        target_end = editor_line_end(buf, len, current_start);
        if (target_end >= len) return;
        target_start = target_end + 1;
        target_end = editor_line_end(buf, len, target_start);
    }

    *cursor = target_start + current_col;
    if (*cursor > target_end) *cursor = target_end;
}

static void editor_scroll_to_cursor(void) {
    uint64_t row = editor_cursor_row(editor_buf, editor_cursor);
    int win_h = gui_window_height();
    int rows = (win_h - ED_TOP_H - ED_STATUS_H) / FONT_CELL_HEIGHT;
    if (rows < 1) rows = 1;
    if (row < editor_top_row) editor_top_row = row;
    if (row >= editor_top_row + (uint64_t)rows) editor_top_row = row - (uint64_t)rows + 1;
}

/* ---- GUI rendering --------------------------------------------------- */

static void ed_draw_text(int x, int y, const char *text, uint32_t fg, uint32_t bg, int max_px) {
    int cx = x;
    if (max_px <= 0) return;
    while (text && *text && cx + FONT_CELL_WIDTH <= x + max_px) {
        gui_draw_char(cx, y, *text, fg, bg);
        cx += FONT_CELL_WIDTH;
        text++;
    }
}

static void ed_draw(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    int rows = (h - ED_TOP_H - ED_STATUS_H) / FONT_CELL_HEIGHT;
    int cols = (w - ED_GUTTER_W - 8) / FONT_CELL_WIDTH;
    uint64_t line_no;
    uint64_t start;

    if (rows < 1) rows = 1;
    if (cols < 8) cols = 8;

    /* Header bar */
    gui_fill_rect(0, 0, w, ED_TOP_H, 0x001A73E8);
    ed_draw_text(14, 10, "ICDA Editor", 0x00FFFFFF, 0x001A73E8, 140);
    ed_draw_text(14, 32, editor_path, 0x00DCE8FA, 0x001A73E8, w - 28);
    ed_draw_text(w - 120, 32, editor_modified ? "*modified" : "saved",
                 0x00FFFFFF, 0x001A73E8, 104);

    /* Text area */
    gui_fill_rect(0, ED_TOP_H, w, h - ED_TOP_H - ED_STATUS_H, 0x00FFFFFF);

    line_no = editor_top_row + 1;
    start = editor_find_row_start(editor_buf, editor_len, editor_top_row);

    for (int r = 0; r < rows; r++) {
        int y = ED_TOP_H + 4 + r * FONT_CELL_HEIGHT;
        uint64_t cursor_row = editor_cursor_row(editor_buf, editor_cursor);
        int is_cursor_line = ((uint64_t)r + editor_top_row) == cursor_row;

        /* Line number gutter */
        gui_fill_rect(0, y, ED_GUTTER_W, FONT_CELL_HEIGHT,
                      is_cursor_line ? 0x00DCE8FA : 0x00F0F2F5);
        {
            char num[16];
            num[0] = 0;
            ed_append_uint(num, line_no, sizeof(num));
            ed_draw_text(ED_GUTTER_W - 6 - (int)ed_strlen(num) * FONT_CELL_WIDTH,
                         y, num, is_cursor_line ? 0x001A73E8 : 0x00878B90,
                         is_cursor_line ? 0x00DCE8FA : 0x00F0F2F5, ED_GUTTER_W - 6);
        }

        if (start > editor_len) start = editor_len;
        {
            uint64_t end = editor_line_end(editor_buf, editor_len, start);
            uint64_t pos = start;
            int cx = ED_GUTTER_W + 4;
            int max_cols = cols;
            while (pos < end && max_cols > 0) {
                uint32_t fg = 0x00202124;
                uint32_t bg = 0x00FFFFFF;
                char ch = editor_buf[pos];
                if (pos == editor_cursor) {
                    /* cursor cell: inverted */
                    fg = 0x00FFFFFF;
                    bg = 0x001A73E8;
                }
                gui_fill_rect(cx, y, FONT_CELL_WIDTH, FONT_CELL_HEIGHT, bg);
                if (ch >= 32 && ch < 127) {
                    gui_draw_char(cx, y, ch, fg, bg);
                } else if (ch == '\t') {
                    /* expand tab to 4 cells */
                    gui_fill_rect(cx, y, FONT_CELL_WIDTH * 3, FONT_CELL_HEIGHT, bg);
                    pos++;
                    cx += FONT_CELL_WIDTH * 4;
                    max_cols -= 4;
                    continue;
                }
                pos++;
                cx += FONT_CELL_WIDTH;
                max_cols--;
            }
            /* cursor at end of line */
            if (pos == editor_cursor && pos == end) {
                gui_fill_rect(cx, y, FONT_CELL_WIDTH, FONT_CELL_HEIGHT, 0x001A73E8);
            }
            /* clear the rest of the line */
            if (cx < w - 4) {
                gui_fill_rect(cx, y, w - 4 - cx, FONT_CELL_HEIGHT, 0x00FFFFFF);
            }
            start = end;
            if (start < editor_len && editor_buf[start] == '\n') start++;
        }
        line_no++;
    }

    /* Status bar */
    gui_fill_rect(0, h - ED_STATUS_H, w, ED_STATUS_H, 0x00E8ECF1);
    gui_draw_hline(0, h - ED_STATUS_H, w, 0x00D0D3D6);
    {
        char sb[96];
        sb[0] = 0;
        ed_copy(sb, "Ln ", sizeof(sb));
        ed_append_uint(sb, editor_line_number(editor_buf, editor_cursor), sizeof(sb));
        ed_draw_text(10, h - ED_STATUS_H + 7, sb, 0x00334455, 0x00E8ECF1, 90);
        {
            char sb2[64];
            sb2[0] = 0;
            ed_copy(sb2, "Col ", sizeof(sb2));
            ed_append_uint(sb2, editor_column(editor_buf, editor_len, editor_cursor) + 1, sizeof(sb2));
            ed_draw_text(110, h - ED_STATUS_H + 7, sb2, 0x00334455, 0x00E8ECF1, 80);
        }
        ed_draw_text(210, h - ED_STATUS_H + 7, status_msg, 0x005F6368, 0x00E8ECF1, w - 220);
    }
}

static void ed_save(void) {
    if ((long)icda_write_file(editor_path, editor_buf, editor_len) >= 0) {
        editor_modified = 0;
        ed_copy(status_msg, "Saved", sizeof(status_msg));
    } else {
        ed_copy(status_msg, "Save failed!", sizeof(status_msg));
    }
}

static void ed_handle_key(uint32_t code, int *key_seq) {
    if (*key_seq == 0 && code == 27) { *key_seq = 1; return; }
    if (*key_seq == 1 && code == '[') { *key_seq = 2; return; }
    if (*key_seq == 2) {
        *key_seq = 0;
        if (code == 'A') { editor_move_vertical(editor_buf, editor_len, &editor_cursor, -1); }
        else if (code == 'B') { editor_move_vertical(editor_buf, editor_len, &editor_cursor, 1); }
        else if (code == 'C') { editor_move_right(editor_len, &editor_cursor); }
        else if (code == 'D') { editor_move_left(&editor_cursor); }
        else if (code == '3') { editor_delete(editor_buf, &editor_len, &editor_cursor); editor_modified = 1; }
        return;
    }
    *key_seq = 0;
    switch (code) {
        case 24: /* Ctrl+X: save+exit */
            ed_save();
            gui_close_window();
            icda_exit(0);
            return;
        case 19: /* Ctrl+S */
            ed_save();
            return;
        case 8: /* backspace */
            editor_backspace(editor_buf, &editor_len, &editor_cursor);
            editor_modified = 1;
            return;
        case 127: /* delete (GUI key 127 = forward delete) */
            editor_delete(editor_buf, &editor_len, &editor_cursor);
            editor_modified = 1;
            return;
        case '\r':
        case '\n':
            editor_insert_char(editor_buf, &editor_len, &editor_cursor, '\n', sizeof(editor_buf));
            editor_modified = 1;
            return;
        default:
            if (code >= 32 && code <= 126) {
                editor_insert_char(editor_buf, &editor_len, &editor_cursor, (char)code, sizeof(editor_buf));
                editor_modified = 1;
            }
            return;
    }
}

static void ed_handle_mouse(gui_msg_t *msg) {
    int mx = msg->mouse.x;
    int my = msg->mouse.y;
    int w = gui_window_width();
    int rows = (gui_window_height() - ED_TOP_H - ED_STATUS_H) / FONT_CELL_HEIGHT;
    if (rows < 1) rows = 1;

    if (my < ED_TOP_H || my >= gui_window_height() - ED_STATUS_H) return;
    {
        uint64_t row = (uint64_t)((my - ED_TOP_H) / FONT_CELL_HEIGHT) + editor_top_row;
        uint64_t col = mx >= ED_GUTTER_W ? (uint64_t)((mx - ED_GUTTER_W) / FONT_CELL_WIDTH) : 0;
        uint64_t start = editor_find_row_start(editor_buf, editor_len, row);
        uint64_t end = editor_line_end(editor_buf, editor_len, start);
        editor_cursor = start + col;
        if (editor_cursor > end) editor_cursor = end;
        (void)w;
    }
}

int main(int argc, char **argv) {
    long ret;
    int key_seq = 0;
    (void)argc;
    (void)argv;

    if (gui_open_window("Editor", 680, 460) != 0) {
        icda_write("editor requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }

    ret = (long)icda_read_file(EDIT_REQUEST_PATH, editor_path, sizeof(editor_path));
    if (ret <= 0) {
        ed_copy(editor_path, EDIT_DEFAULT_PATH, sizeof(editor_path));
        editor_buf[0] = 0;
        editor_len = 0;
    } else {
        if ((uint64_t)ret >= sizeof(editor_path)) ret = (long)sizeof(editor_path) - 1;
        editor_path[ret] = 0;
        ret = (long)icda_read_file(editor_path, editor_buf, sizeof(editor_buf) - 1);
        if (ret < 0) {
            editor_buf[0] = 0;
            editor_len = 0;
        } else {
            editor_len = (uint64_t)ret;
            editor_buf[editor_len] = 0;
        }
    }

    ed_draw();
    gui_flush();

    for (;;) {
        gui_msg_t msg;
        int changed = 0;
        while (gui_poll_event(&msg)) {
            changed = 1;
            if (msg.type == GUI_MSG_MOUSE_EVENT && (msg.mouse.buttons & GUI_BTN_LEFT)) {
                ed_handle_mouse(&msg);
            } else if (msg.type == GUI_MSG_KEY_EVENT && msg.key.pressed) {
                ed_handle_key(msg.key.keycode, &key_seq);
            } else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                gui_close_window();
                return 0;
            }
        }
        if (changed) {
            editor_scroll_to_cursor();
            ed_draw();
            gui_flush();
        }
        icda_sleep(1);
    }
}
