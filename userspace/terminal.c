#include "gui.h"
#include "icda_sys.h"

#define GRID_ROWS 18
#define GRID_COLS 62

static char grid[GRID_ROWS][GRID_COLS];
static uint32_t fg_grid[GRID_ROWS][GRID_COLS];
static uint32_t bg_grid[GRID_ROWS][GRID_COLS];
static int cursor_col = 0;
static int cursor_row = 0;
static char cmd_buf[128];

static int str_prefix(const char *text, const char *prefix) {
    int i = 0;
    while (prefix[i]) {
        if (text[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static void scroll_up(void) {
    for (int r = 0; r < GRID_ROWS - 1; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            grid[r][c] = grid[r + 1][c];
            fg_grid[r][c] = fg_grid[r + 1][c];
            bg_grid[r][c] = bg_grid[r + 1][c];
        }
    }
    for (int c = 0; c < GRID_COLS; c++) {
        grid[GRID_ROWS - 1][c] = ' ';
        fg_grid[GRID_ROWS - 1][c] = 0x00F8FAFC;
        bg_grid[GRID_ROWS - 1][c] = 0x000F172A;
    }
    cursor_row = GRID_ROWS - 1;
}

static void term_putc(char c, uint32_t fg, uint32_t bg) {
    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= GRID_ROWS) {
            scroll_up();
        }
        return;
    }
    if (c == '\r') {
        cursor_col = 0;
        return;
    }
    if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            grid[cursor_row][cursor_col] = ' ';
        }
        return;
    }

    grid[cursor_row][cursor_col] = c;
    fg_grid[cursor_row][cursor_col] = fg;
    bg_grid[cursor_row][cursor_col] = bg;
    cursor_col++;
    if (cursor_col >= GRID_COLS) {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= GRID_ROWS) {
            scroll_up();
        }
    }
}

static void term_print(const char *str, uint32_t fg, uint32_t bg) {
    while (*str) {
        term_putc(*str, fg, bg);
        str++;
    }
}

static void render_grid(void) {
    /* Rich dark theme workspace background */
    gui_fill_rect(0, 0, 500, 300, 0x000F172A);
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            char ch = grid[r][c];
            if (ch != ' ' && ch != '\0') {
                gui_draw_char(c * 8, r * 16, ch, fg_grid[r][c], bg_grid[r][c]);
            }
        }
    }
    /* Sleek light cyan cursor block */
    gui_fill_rect(cursor_col * 8, cursor_row * 16, 8, 16, 0x0038BDF8);
    gui_flush();
}

static void term_execute(const char *cmd) {
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    if (str_prefix(cmd, "help")) {
        term_print("Available commands:\n", 0x0038BDF8, 0x000F172A);
        term_print("  help              Show this help menu\n", 0x00F8FAFC, 0x000F172A);
        term_print("  ls                List files in current directory\n", 0x00F8FAFC, 0x000F172A);
        term_print("  cat <file>        Print content of a file\n", 0x00F8FAFC, 0x000F172A);
        term_print("  clear             Clear the terminal screen\n", 0x00F8FAFC, 0x000F172A);
        term_print("  run <app>         Execute a userspace app\n", 0x00F8FAFC, 0x000F172A);
    }
    else if (str_prefix(cmd, "clear")) {
        for (int r = 0; r < GRID_ROWS; r++) {
            for (int c = 0; c < GRID_COLS; c++) {
                grid[r][c] = ' ';
                fg_grid[r][c] = 0x00F8FAFC;
                bg_grid[r][c] = 0x000F172A;
            }
        }
        cursor_col = 0;
        cursor_row = 0;
    }
    else if (str_prefix(cmd, "ls")) {
        char buf[2048];
        uint64_t rc = icda_list_dir(".", buf, sizeof(buf));
        if (rc == 0 || rc == (uint64_t)-1) {
            term_print("Empty directory or error\n", 0x00EF4444, 0x000F172A);
        } else {
            term_print(buf, 0x00F8FAFC, 0x000F172A);
            term_print("\n", 0x00F8FAFC, 0x000F172A);
        }
    }
    else if (str_prefix(cmd, "cat ")) {
        const char *filename = cmd + 4;
        while (*filename == ' ') filename++;
        char buf[4096];
        uint64_t rc = icda_read_file(filename, buf, sizeof(buf) - 1);
        if (rc == (uint64_t)-1) {
            term_print("Error: Could not read file\n", 0x00EF4444, 0x000F172A);
        } else {
            buf[rc] = '\0';
            term_print(buf, 0x00F8FAFC, 0x000F172A);
            term_print("\n", 0x00F8FAFC, 0x000F172A);
        }
    }
    else if (str_prefix(cmd, "run ")) {
        const char *app = cmd + 4;
        while (*app == ' ') app++;
        term_print("Launching ", 0x0038BDF8, 0x000F172A);
        term_print(app, 0x00F8FAFC, 0x000F172A);
        term_print("...\n", 0x0038BDF8, 0x000F172A);
        uint64_t pid = icda_spawn(app);
        if (pid == 0 || (int64_t)pid < 0) {
            term_print("Failed to launch application\n", 0x00EF4444, 0x000F172A);
        }
    }
    else {
        uint64_t pid = icda_spawn(cmd);
        if (pid == 0 || (int64_t)pid < 0) {
            term_print("Command not found. Type 'help' for options.\n", 0x00EF4444, 0x000F172A);
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (gui_open_window("Terminal", 500, 300) != 0) {
        return -1;
    }

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            grid[r][c] = ' ';
            fg_grid[r][c] = 0x00F8FAFC;
            bg_grid[r][c] = 0x000F172A;
        }
    }

    term_print("ICDA GUI Terminal v1.0\nType 'help' for commands.\n\n", 0x0038BDF8, 0x000F172A);
    term_print("icda@desktop:$ ", 0x0010B981, 0x000F172A); /* Cyan/Green prompt */
    render_grid();

    int cmd_len = 0;
    cmd_buf[0] = '\0';

    for (;;) {
        gui_msg_t msg;
        gui_wait_event(&msg);

        if (msg.type == GUI_MSG_KEY_EVENT) {
            uint32_t code = msg.key.keycode;
            if (msg.key.pressed) {
                if (code == '\n') {
                    term_putc('\n', 0x00F8FAFC, 0x000F172A);
                    cmd_buf[cmd_len] = '\0';
                    term_execute(cmd_buf);
                    cmd_len = 0;
                    cmd_buf[0] = '\0';
                    term_print("icda@desktop:$ ", 0x0010B981, 0x000F172A);
                }
                else if (code == '\b') {
                    if (cmd_len > 0) {
                        cmd_len--;
                        cmd_buf[cmd_len] = '\0';
                        term_putc('\b', 0x00F8FAFC, 0x000F172A);
                    }
                }
                else if (code >= 32 && code <= 126) {
                    if (cmd_len < 120) {
                        cmd_buf[cmd_len++] = (char)code;
                        cmd_buf[cmd_len] = '\0';
                        term_putc((char)code, 0x00F8FAFC, 0x000F172A);
                    }
                }
                render_grid();
            }
        }
        else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
            break;
        }
    }

    gui_close_window();
    return 0;
}
