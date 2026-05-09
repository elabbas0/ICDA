#include "tty.h"

#include "../drivers/console/console.h"
#include "../drivers/input/input.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../memory/pmm.h"

#include <stddef.h>
#include <stdint.h>

#define TTY_INITIAL_CAP  128U

static char *tty_line = 0;
static uint64_t tty_len = 0;
static uint64_t tty_cap = 0;
static int tty_cursor_visible = 0;
static vfs_node_t *tty_cwd = 0;
static char tty_prompt_buf[96];

static int streq(const char *a, const char *b) {
    uint64_t i = 0;

    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }

    return a[i] == '\0' && b[i] == '\0';
}

static void tty_hide_cursor(void) {
    if (!tty_cursor_visible) {
        return;
    }

    console_backspace();
    tty_cursor_visible = 0;
}

static void tty_show_cursor(void) {
    if (tty_cursor_visible) {
        return;
    }

    console_write_char('_', CONSOLE_STYLE_ACCENT);
    tty_cursor_visible = 1;
}

static void tty_refresh_prompt(void) {
    char cwd[80];
    uint64_t out = 0;
    const char *prefix = "icda:";
    const char *fallback = "icda:/ ";

    if (!tty_cwd || vfs_getcwd(tty_cwd, cwd, sizeof(cwd)) != 0) {
        for (uint64_t i = 0; fallback[i] && i + 1 < sizeof(tty_prompt_buf); i++) {
            tty_prompt_buf[i] = fallback[i];
            tty_prompt_buf[i + 1] = '\0';
        }
        return;
    }

    while (prefix[out] && out + 1 < sizeof(tty_prompt_buf)) {
        tty_prompt_buf[out] = prefix[out];
        out++;
    }
    for (uint64_t i = 0; cwd[i] && out + 1 < sizeof(tty_prompt_buf); i++) {
        tty_prompt_buf[out++] = cwd[i];
    }
    if (out + 2 < sizeof(tty_prompt_buf)) {
        tty_prompt_buf[out++] = ' ';
        tty_prompt_buf[out] = '\0';
    } else {
        tty_prompt_buf[sizeof(tty_prompt_buf) - 1] = '\0';
    }
}

static void tty_prompt(void) {
    tty_refresh_prompt();
    console_write(tty_prompt_buf, CONSOLE_STYLE_ACCENT);
    tty_show_cursor();
}

static int tty_grow_line(uint64_t needed) {
    char *next;
    uint64_t new_cap = tty_cap ? tty_cap : TTY_INITIAL_CAP;

    while (new_cap < needed) {
        new_cap *= 2;
    }

    next = (char *)krealloc(tty_line, (size_t)new_cap);
    if (!next) {
        console_write("\nout of heap memory\n", CONSOLE_STYLE_ERROR);
        return -1;
    }

    tty_line = next;
    tty_cap = new_cap;
    return 0;
}

static void tty_reset_line(void) {
    tty_len = 0;
    if (tty_line && tty_cap) {
        tty_line[0] = '\0';
    }
}

static void tty_print_help(void) {
    console_write("commands: help clear mem pwd cd ls cat mkdir touch write stat echo reboot\n", CONSOLE_STYLE_MUTED);
}

static void tty_print_mem(void) {
    console_write("heap total=", CONSOLE_STYLE_INFO);
    console_write_dec64(heap_bytes_total(), CONSOLE_STYLE_INFO);
    console_write(" used=", CONSOLE_STYLE_INFO);
    console_write_dec64(heap_bytes_used(), CONSOLE_STYLE_INFO);
    console_write(" free_frames=", CONSOLE_STYLE_INFO);
    console_write_dec64(pmm_free_frames(), CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);
}

static uint64_t tty_strlen(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) {
        len++;
    }
    return len;
}

static void tty_print_file(const char *data, uint64_t size) {
    console_write(data, CONSOLE_STYLE_INFO);
    if (size == 0 || data[size - 1] != '\n') {
        console_write("\n", CONSOLE_STYLE_INFO);
    }
}

static void tty_reboot(void) {
    uint8_t status;

    console_write("rebooting...\n", CONSOLE_STYLE_WARN);
    do {
        __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x64));
    } while (status & 0x02);
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

static char *tty_trim(char *text) {
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

static void tty_print_path(vfs_node_t *node) {
    char cwd[80];
    if (vfs_getcwd(node, cwd, sizeof(cwd)) != 0) {
        console_write("/\n", CONSOLE_STYLE_INFO);
        return;
    }
    console_write(cwd, CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);
}

static void tty_list_dir(const char *path) {
    vfs_node_t *dir = path && *path ? vfs_resolve(tty_cwd, path) : tty_cwd;
    uint64_t count;

    if (!dir || vfs_node_type(dir) != VFS_NODE_DIR) {
        console_write("directory not found\n", CONSOLE_STYLE_WARN);
        return;
    }

    count = vfs_child_count(dir);
    for (uint64_t i = 0; i < count; i++) {
        vfs_node_t *child = vfs_child_at(dir, i);
        const char *name = vfs_node_name(child);
        console_write(name, CONSOLE_STYLE_MUTED);
        if (vfs_node_type(child) == VFS_NODE_DIR) {
            console_write("/", CONSOLE_STYLE_ACCENT);
        }
        console_write("\n", CONSOLE_STYLE_INFO);
    }
}

static void tty_print_stat(const char *path) {
    vfs_stat_t stat;

    if (!path || !*path || vfs_stat(tty_cwd, path, &stat) != 0) {
        console_write("path not found\n", CONSOLE_STYLE_WARN);
        return;
    }

    console_write("inode=", CONSOLE_STYLE_MUTED);
    console_write_dec64(stat.inode, CONSOLE_STYLE_INFO);
    console_write(" type=", CONSOLE_STYLE_MUTED);
    console_write(stat.type == VFS_NODE_DIR ? "dir" : "file", CONSOLE_STYLE_INFO);
    console_write(" size=", CONSOLE_STYLE_MUTED);
    console_write_dec64(stat.size, CONSOLE_STYLE_INFO);
    console_write(" created=", CONSOLE_STYLE_MUTED);
    console_write_dec64(stat.created, CONSOLE_STYLE_INFO);
    console_write(" modified=", CONSOLE_STYLE_MUTED);
    console_write_dec64(stat.modified, CONSOLE_STYLE_INFO);
    console_write(" readonly=", CONSOLE_STYLE_MUTED);
    console_write(stat.readonly ? "yes" : "no", CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);
}

static void tty_dispatch_line(void) {
    char *line = tty_line;
    char *arg = 0;

    while (*line == ' ' || *line == '\t') {
        line++;
    }

    for (uint64_t i = 0; line[i]; i++) {
        if (line[i] == ' ' || line[i] == '\t') {
            line[i] = '\0';
            arg = &line[i + 1];
            while (*arg == ' ' || *arg == '\t') {
                arg++;
            }
            break;
        }
    }

    if (*line == '\0') {
        return;
    }
    if (streq(line, "help")) {
        tty_print_help();
        return;
    }
    if (streq(line, "clear")) {
        console_clear();
        return;
    }
    if (streq(line, "mem")) {
        tty_print_mem();
        return;
    }
    if (streq(line, "pwd")) {
        tty_print_path(tty_cwd);
        return;
    }
    if (streq(line, "cd")) {
        vfs_node_t *next;

        if (!arg || !*arg) {
            tty_cwd = vfs_root();
            return;
        }
        next = vfs_resolve(tty_cwd, arg);
        if (!next || vfs_node_type(next) != VFS_NODE_DIR) {
            console_write("directory not found: ", CONSOLE_STYLE_ERROR);
            console_write(arg, CONSOLE_STYLE_ERROR);
            console_write("\n", CONSOLE_STYLE_ERROR);
            return;
        }
        tty_cwd = next;
        return;
    }
    if (streq(line, "ls")) {
        tty_list_dir(arg);
        return;
    }
    if (streq(line, "cat")) {
        const char *data;
        uint64_t size = 0;

        if (!arg || !*arg) {
            console_write("usage: cat <path>\n", CONSOLE_STYLE_WARN);
            return;
        }

        data = vfs_read(tty_cwd, arg, &size);
        if (!data) {
            console_write("file not found: ", CONSOLE_STYLE_ERROR);
            console_write(arg, CONSOLE_STYLE_ERROR);
            console_write("\n", CONSOLE_STYLE_ERROR);
            return;
        }

        tty_print_file(data, size);
        return;
    }
    if (streq(line, "mkdir")) {
        if (!arg || !*arg || vfs_mkdir(tty_cwd, arg) != 0) {
            console_write("mkdir failed\n", CONSOLE_STYLE_WARN);
            return;
        }
        return;
    }
    if (streq(line, "touch")) {
        if (!arg || !*arg || vfs_create(tty_cwd, arg) != 0) {
            console_write("touch failed\n", CONSOLE_STYLE_WARN);
            return;
        }
        return;
    }
    if (streq(line, "write")) {
        char *path;
        char *text;

        if (!arg || !*arg) {
            console_write("usage: write <path> <text>\n", CONSOLE_STYLE_WARN);
            return;
        }

        path = arg;
        while (*arg && *arg != ' ' && *arg != '\t') {
            arg++;
        }
        if (*arg) {
            *arg++ = '\0';
        }
        text = tty_trim(arg);
        if (vfs_write(tty_cwd, path, text, tty_strlen(text)) != 0) {
            console_write("write failed\n", CONSOLE_STYLE_WARN);
        }
        return;
    }
    if (streq(line, "stat")) {
        if (!arg || !*arg) {
            console_write("usage: stat <path>\n", CONSOLE_STYLE_WARN);
            return;
        }
        tty_print_stat(arg);
        return;
    }
    if (streq(line, "echo")) {
        console_write(arg ? arg : "", CONSOLE_STYLE_INFO);
        console_write("\n", CONSOLE_STYLE_INFO);
        return;
    }
    if (streq(line, "reboot")) {
        tty_reboot();
        return;
    }

    console_write("unknown command: ", CONSOLE_STYLE_ERROR);
    console_write(line, CONSOLE_STYLE_ERROR);
    console_write("\n", CONSOLE_STYLE_ERROR);
}

static void tty_submit_line(void) {
    tty_hide_cursor();
    console_write("\n", CONSOLE_STYLE_INFO);
    tty_line[tty_len] = '\0';
    tty_dispatch_line();
    tty_reset_line();
    tty_prompt();
}

static void tty_backspace(void) {
    if (tty_len == 0) {
        tty_show_cursor();
        return;
    }

    tty_len--;
    tty_line[tty_len] = '\0';
    console_backspace();
    tty_show_cursor();
}

int tty_init(void) {
    const char *motd;
    uint64_t size = 0;

    if (tty_grow_line(TTY_INITIAL_CAP) != 0) {
        return -1;
    }

    tty_reset_line();
    tty_cursor_visible = 0;
    tty_cwd = vfs_root();
    motd = vfs_read(tty_cwd, "/etc/motd.txt", &size);
    if (motd) {
        tty_print_file(motd, size);
        console_write("\n", CONSOLE_STYLE_INFO);
    }
    tty_prompt();
    return 0;
}

void tty_poll(void) {
    int c = input_read_char();

    if (c < 0) {
        return;
    }

    tty_hide_cursor();

    if (c == '\b') {
        tty_backspace();
        return;
    }

    if (c == '\n' || c == '\r') {
        tty_submit_line();
        return;
    }

    if (c == '\t') {
        c = ' ';
    }

    if (c < 32 || c > 126) {
        tty_show_cursor();
        return;
    }

    if (tty_grow_line(tty_len + 2) != 0) {
        tty_show_cursor();
        return;
    }

    tty_line[tty_len++] = (char)c;
    tty_line[tty_len] = '\0';
    console_write_char((char)c, CONSOLE_STYLE_INFO);
    tty_show_cursor();
}
