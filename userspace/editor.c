#include "icda_sys.h"

#include <stdint.h>

#define EDIT_REQUEST_PATH "/home/.edit.request"
#define EDIT_PATH_CAP 128
#define EDIT_BUF_CAP 4096
#define EDIT_MAX_ROWS 48
#define EDIT_MAX_COLS 128
#define CP437_BLOCK_MED ((char)0xB2)

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
static uint64_t editor_cols = 80;
static uint64_t editor_rows = 25;
static uint64_t editor_view_rows = 18;
static uint64_t editor_view_cols = 68;
static uint64_t editor_top = 1;
static char editor_prev_lines[EDIT_MAX_ROWS][EDIT_MAX_COLS + 1];
static int editor_prev_valid = 0;

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void copy_text(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!cap) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void line_reset(char *line, uint64_t width) {
    for (uint64_t i = 0; i < width; i++) line[i] = ' ';
    line[width] = 0;
}

static void line_put_text(char *line, uint64_t width, uint64_t col, const char *text) {
    uint64_t i = 0;
    while (text && text[i] && col + i < width) {
        line[col + i] = text[i];
        i++;
    }
}

static void line_put_uint(char *line, uint64_t width, uint64_t col, uint64_t value, uint64_t min_width) {
    char buf[32];
    uint64_t i = sizeof(buf) - 1;
    uint64_t len;

    buf[i] = 0;
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value && i > 0) {
            buf[--i] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    len = str_len(&buf[i]);
    while (len < min_width && col < width) {
        line[col++] = ' ';
        len++;
    }
    while (buf[i] && col < width) {
        line[col++] = buf[i++];
    }
}

static long editor_wait_key_byte(uint64_t timeout_ticks) {
    return icda_read_char_timeout(timeout_ticks);
}

static long editor_read_key(void) {
    long c = editor_wait_key_byte(0);
    if (c != 27) return c;
    {
        long c1 = editor_wait_key_byte(2);
        if (c1 != '[') return c;
        switch (editor_wait_key_byte(2)) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case '3':
                if (editor_wait_key_byte(2) == '~') return KEY_DELETE;
                return c;
            default: return c;
        }
    }
}

static void editor_layout_refresh(void) {
    uint64_t cols = 80;
    uint64_t rows = 25;

    if (icda_console_size(&cols, &rows) != 0) {
        cols = 80;
        rows = 25;
    }
    if (cols > EDIT_MAX_COLS) cols = EDIT_MAX_COLS;
    if (rows > EDIT_MAX_ROWS) rows = EDIT_MAX_ROWS;
    if (cols < 32) cols = 32;
    if (rows < 10) rows = 10;
    editor_cols = cols;
    editor_rows = rows > 1 ? rows - 1 : rows;
    editor_view_rows = editor_rows > 6 ? editor_rows - 6 : 1;
    editor_view_cols = cols > 8 ? cols - 8 : 8;
}

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

static uint64_t editor_line_count(const char *buf, uint64_t len) {
    uint64_t lines = 1;
    for (uint64_t i = 0; i < len; i++) if (buf[i] == '\n') lines++;
    return lines;
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

static uint64_t editor_view_top_for_cursor(const char *buf, uint64_t cursor) {
    uint64_t row = editor_cursor_row(buf, cursor);
    if (row < 2) return 0;
    if (row + 3 < editor_view_rows) return 0;
    return row - 2;
}

static void editor_move_row(uint64_t row) {
    icda_set_cursor(0, editor_top + row);
}

static void editor_write_line_segment(const char *buf, uint64_t len, uint64_t start, uint64_t cursor, uint64_t *cursor_shown, char *line, uint64_t width, uint64_t col) {
    uint64_t end = editor_line_end(buf, len, start);
    uint64_t pos = start;
    uint64_t shown = 0;

    while (pos < end) {
        if (shown >= editor_view_cols || col + shown >= width) break;
        if (pos == cursor && !*cursor_shown) {
            line[col + shown] = '|';
            *cursor_shown = 1;
            shown++;
            if (shown >= editor_view_cols || col + shown >= width) break;
        }
        line[col + shown] = buf[pos];
        shown++;
        pos++;
    }
    if (cursor == end && !*cursor_shown && shown < editor_view_cols && col + shown < width) {
        line[col + shown] = '|';
        *cursor_shown = 1;
    }
}

static void editor_emit_line(uint64_t row, const char *line) {
    editor_move_row(row);
    icda_write(line);
}

static void editor_redraw(const char *path, const char *buf, uint64_t len, uint64_t cursor, int modified) {
    uint64_t top_row = editor_view_top_for_cursor(buf, cursor);
    uint64_t start = editor_find_row_start(buf, len, top_row);
    uint64_t line_no = top_row + 1;
    uint64_t cursor_shown = 0;
    uint64_t lines_total = editor_line_count(buf, len);
    char lines[EDIT_MAX_ROWS][EDIT_MAX_COLS + 1];

    editor_layout_refresh();

    for (uint64_t row = 0; row < editor_rows; row++) {
        line_reset(lines[row], editor_cols);
    }

    line_put_text(lines[0], editor_cols, 0, " ICDA Editor  ");
    line_put_text(lines[0], editor_cols, 14, path);
    if (editor_cols > 12) {
        uint64_t state_col = editor_cols > 18 ? editor_cols - 18 : 0;
        line_put_text(lines[0], editor_cols, state_col, modified ? "*modified" : "saved");
    }
    for (uint64_t i = 0; i < editor_cols; i++) lines[1][i] = CP437_BLOCK_MED;
    line_put_text(lines[2], editor_cols, 0, " Ctrl+S save   Ctrl+X exit   arrows move   backspace/delete");

    for (uint64_t row = 0; row < editor_view_rows && 3 + row < editor_rows - 2; row++) {
        uint64_t target_row = 3 + row;
        if (start > len) start = len;
        line_put_uint(lines[target_row], editor_cols, 0, line_no, 4);
        line_put_text(lines[target_row], editor_cols, 4, "  ");
        if (start <= len) {
            editor_write_line_segment(buf, len, start, cursor, &cursor_shown, lines[target_row], editor_cols, 6);
            start = editor_line_end(buf, len, start);
            if (start < len && buf[start] == '\n') start++;
        }
        line_no++;
    }

    if (editor_rows >= 2) {
        for (uint64_t i = 0; i < editor_cols; i++) lines[editor_rows - 2][i] = CP437_BLOCK_MED;
    }
    if (editor_rows >= 1) {
        uint64_t status_row = editor_rows - 1;
        uint64_t col = 0;
        line_put_text(lines[status_row], editor_cols, col, " Ln ");
        col = str_len(lines[status_row]);
        line_put_uint(lines[status_row], editor_cols, col, editor_line_number(buf, cursor), 0);
        col = str_len(lines[status_row]);
        line_put_text(lines[status_row], editor_cols, col, "/");
        col++;
        line_put_uint(lines[status_row], editor_cols, col, lines_total, 0);
        col = str_len(lines[status_row]);
        line_put_text(lines[status_row], editor_cols, col, "   Col ");
        col += 7;
        line_put_uint(lines[status_row], editor_cols, col, editor_column(buf, len, cursor) + 1, 0);
        col = str_len(lines[status_row]);
        line_put_text(lines[status_row], editor_cols, col, "   Size ");
        col += 8;
        line_put_uint(lines[status_row], editor_cols, col, len, 0);
        col = str_len(lines[status_row]);
        line_put_text(lines[status_row], editor_cols, col, " bytes");
        if (!cursor_shown) {
            col = str_len(lines[status_row]);
            line_put_text(lines[status_row], editor_cols, col, "   [cursor off-screen]");
        }
    }

    for (uint64_t row = 0; row < editor_rows; row++) {
        int changed = !editor_prev_valid;
        if (!changed) {
            for (uint64_t col = 0; col < editor_cols; col++) {
                if (editor_prev_lines[row][col] != lines[row][col]) {
                    changed = 1;
                    break;
                }
            }
        }
        if (changed) {
            editor_emit_line(row, lines[row]);
            copy_text(editor_prev_lines[row], lines[row], sizeof(editor_prev_lines[row]));
        }
    }
    editor_prev_valid = 1;
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

uint64_t editor_main(void) {
    long ret;
    uint64_t len = 0;
    uint64_t cursor = 0;
    int modified = 0;

    ret = (long)icda_read_file(EDIT_REQUEST_PATH, editor_path, sizeof(editor_path));
    if (ret <= 0) {
        icda_write("editor: no request path\n");
        icda_exit(1);
    }
    if ((uint64_t)ret >= sizeof(editor_path)) ret = (long)sizeof(editor_path) - 1;
    editor_path[ret] = 0;

    ret = (long)icda_read_file(editor_path, editor_buf, sizeof(editor_buf) - 1);
    if (ret < 0) {
        editor_buf[0] = 0;
        len = 0;
    } else {
        len = (uint64_t)ret;
        editor_buf[len] = 0;
    }

    icda_clear();
    editor_prev_valid = 0;
    editor_redraw(editor_path, editor_buf, len, cursor, modified);
    for (;;) {
        long c = editor_read_key();
        if (c < 0) continue;
        if (c == 24) {
            icda_clear();
            icda_exit(0);
        }
        if (c == 19) {
            if ((long)icda_write_file(editor_path, editor_buf, len) >= 0) modified = 0;
            editor_redraw(editor_path, editor_buf, len, cursor, modified);
            continue;
        }
        if (c == '\b') {
            editor_backspace(editor_buf, &len, &cursor);
            modified = 1;
            editor_redraw(editor_path, editor_buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_DELETE) {
            editor_delete(editor_buf, &len, &cursor);
            modified = 1;
            editor_redraw(editor_path, editor_buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_LEFT) {
            editor_move_left(&cursor);
            editor_redraw(editor_path, editor_buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_RIGHT) {
            editor_move_right(len, &cursor);
            editor_redraw(editor_path, editor_buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_UP) {
            editor_move_vertical(editor_buf, len, &cursor, -1);
            editor_redraw(editor_path, editor_buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_DOWN) {
            editor_move_vertical(editor_buf, len, &cursor, 1);
            editor_redraw(editor_path, editor_buf, len, cursor, modified);
            continue;
        }
        if (c == '\r' || c == '\n') {
            editor_insert_char(editor_buf, &len, &cursor, '\n', sizeof(editor_buf));
            modified = 1;
            editor_redraw(editor_path, editor_buf, len, cursor, modified);
            continue;
        }
        if (c >= 32 && c <= 126) {
            editor_insert_char(editor_buf, &len, &cursor, (char)c, sizeof(editor_buf));
            modified = 1;
            editor_redraw(editor_path, editor_buf, len, cursor, modified);
        }
    }
}
