#include "icda_sys.h"

#include <stdint.h>

#define SHELL_LINE_CAP 128
#define SHELL_BUF_CAP 1024

static int shell_cursor_visible = 0;

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static int str_has_slash(const char *s) {
    for (uint64_t i = 0; s && s[i]; i++) if (s[i] == '/') return 1;
    return 0;
}

static void append_text(char *dst, const char *src, uint64_t cap) {
    uint64_t out = str_len(dst);
    uint64_t i = 0;
    while (src && src[i] && out + 1 < cap) dst[out++] = src[i++];
    dst[out] = 0;
}

static void write_char(char c) {
    char buf[2];
    buf[0] = c;
    buf[1] = 0;
    icda_write(buf);
}

static void shell_hide_cursor(void) {
    if (!shell_cursor_visible) return;
    icda_backspace();
    shell_cursor_visible = 0;
}

static void shell_show_cursor(void) {
    if (shell_cursor_visible) return;
    icda_write("_");
    shell_cursor_visible = 1;
}

static void write_uint(uint64_t v) {
    char buf[32];
    uint64_t i = sizeof(buf) - 1;
    buf[i] = 0;
    if (v == 0) {
        icda_write("0");
        return;
    }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    icda_write(&buf[i]);
}

static void print_prompt(void) {
    char cwd[80];
    if ((long)icda_getcwd(cwd, sizeof(cwd)) < 0) {
        icda_write("icda:/ ");
        return;
    }
    icda_write("icda:");
    icda_write(cwd);
    icda_write(" ");
    shell_show_cursor();
}

static void shell_help(void) {
    icda_write("user commands: help clear pid ps pwd cd ls cat mkdir touch write stat run spawn wait exit\n");
}

static void shell_pwd(void) {
    char cwd[80];
    if ((long)icda_getcwd(cwd, sizeof(cwd)) < 0) {
        icda_write("/\n");
        return;
    }
    icda_write(cwd);
    icda_write("\n");
}

static void shell_ps(void) {
    char buf[SHELL_BUF_CAP];
    long ret = (long)icda_list_procs(buf, sizeof(buf));
    if (ret < 0) {
        icda_write("ps failed\n");
        return;
    }
    icda_write(buf);
}

static void shell_ls(const char *path) {
    char buf[SHELL_BUF_CAP];
    const char *target = (path && *path) ? path : ".";
    long ret = (long)icda_list_dir(target, buf, sizeof(buf));
    if (ret < 0) {
        icda_write("ls failed\n");
        return;
    }
    icda_write(buf);
}

static void shell_cat(const char *path) {
    char buf[SHELL_BUF_CAP];
    long ret;
    if (!path || !*path) {
        icda_write("usage: cat <path>\n");
        return;
    }
    ret = (long)icda_read_file(path, buf, sizeof(buf));
    if (ret < 0) {
        icda_write("cat failed\n");
        return;
    }
    icda_write(buf);
    if (ret == 0 || buf[ret - 1] != '\n') icda_write("\n");
}

static void shell_run_path(const char *path) {
    uint64_t pid;
    uint64_t code;
    if (!path || !*path) {
        icda_write("usage: run <path>\n");
        return;
    }
    pid = icda_spawn(path);
    if ((long)pid < 0) {
        icda_write("run failed: ");
        icda_write(path);
        icda_write("\n");
        return;
    }
    code = icda_waitpid(pid);
    if ((long)code < 0) {
        icda_write("wait failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    icda_write("pid=");
    write_uint(pid);
    icda_write(" ");
    icda_write("exit=");
    write_uint(code);
    icda_write("\n");
}

static void shell_mkdir(const char *path) {
    if (!path || !*path) {
        icda_write("usage: mkdir <path>\n");
        return;
    }
    if ((long)icda_mkdir(path) < 0) {
        icda_write("mkdir failed\n");
    }
}

static void shell_touch(const char *path) {
    if (!path || !*path) {
        icda_write("usage: touch <path>\n");
        return;
    }
    if ((long)icda_create(path) < 0) {
        icda_write("touch failed\n");
    }
}

static void shell_write_file(const char *arg) {
    char *text;
    long ret;

    if (!arg || !*arg) {
        icda_write("usage: write <path> <text>\n");
        return;
    }

    text = (char *)arg;
    while (*text && *text != ' ' && *text != '\t') text++;
    if (!*text) {
        icda_write("usage: write <path> <text>\n");
        return;
    }

    *text++ = 0;
    while (*text == ' ' || *text == '\t') text++;
    ret = (long)icda_write_file(arg, text, str_len(text));
    if (ret < 0) {
        icda_write("write failed\n");
    }
}

static void shell_stat(const char *path) {
    icda_stat_t st;

    if (!path || !*path) {
        icda_write("usage: stat <path>\n");
        return;
    }
    if ((long)icda_stat(path, &st) < 0) {
        icda_write("stat failed\n");
        return;
    }

    icda_write("inode=");
    write_uint(st.inode);
    icda_write(" type=");
    icda_write(st.type == 2 ? "dir" : "file");
    icda_write(" size=");
    write_uint(st.size);
    icda_write(" created=");
    write_uint(st.created);
    icda_write(" modified=");
    write_uint(st.modified);
    icda_write(" readonly=");
    icda_write(st.readonly ? "yes" : "no");
    icda_write("\n");
}

static void shell_spawn_path(const char *path) {
    uint64_t pid;
    if (!path || !*path) {
        icda_write("usage: spawn <path>\n");
        return;
    }
    pid = icda_spawn(path);
    if ((long)pid < 0) {
        icda_write("spawn failed: ");
        icda_write(path);
        icda_write("\n");
        return;
    }
    icda_write("spawned pid=");
    write_uint(pid);
    icda_write("\n");
}

static void shell_wait_pid(const char *arg) {
    uint64_t pid = 0;
    uint64_t code;
    uint64_t i = 0;

    if (!arg || !*arg) {
        icda_write("usage: wait <pid>\n");
        return;
    }
    while (arg[i]) {
        if (arg[i] < '0' || arg[i] > '9') {
            icda_write("usage: wait <pid>\n");
            return;
        }
        pid = pid * 10 + (uint64_t)(arg[i] - '0');
        i++;
    }
    code = icda_waitpid(pid);
    if ((long)code < 0) {
        icda_write("wait failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    icda_write("pid=");
    write_uint(pid);
    icda_write(" exit=");
    write_uint(code);
    icda_write("\n");
}

static int shell_try_exec_command(const char *cmd) {
    char path[160];
    uint64_t pid = icda_spawn(cmd);
    if ((long)pid >= 0) {
        uint64_t code = icda_waitpid(pid);
        if ((long)code < 0) {
            icda_write("wait failed: ");
            write_uint(pid);
            icda_write("\n");
            return 1;
        }
        icda_write("pid=");
        write_uint(pid);
        icda_write(" exit=");
        write_uint(code);
        icda_write("\n");
        return 1;
    }
    if (str_has_slash(cmd)) return 0;

    path[0] = 0;
    append_text(path, "/apps/", sizeof(path));
    append_text(path, cmd, sizeof(path));
    pid = icda_spawn(path);
    if ((long)pid >= 0) {
        uint64_t code = icda_waitpid(pid);
        if ((long)code < 0) {
            icda_write("wait failed: ");
            write_uint(pid);
            icda_write("\n");
            return 1;
        }
        icda_write("pid=");
        write_uint(pid);
        icda_write(" exit=");
        write_uint(code);
        icda_write("\n");
        return 1;
    }

    path[0] = 0;
    append_text(path, "/bin/", sizeof(path));
    append_text(path, cmd, sizeof(path));
    pid = icda_spawn(path);
    if ((long)pid >= 0) {
        uint64_t code = icda_waitpid(pid);
        if ((long)code < 0) {
            icda_write("wait failed: ");
            write_uint(pid);
            icda_write("\n");
            return 1;
        }
        icda_write("pid=");
        write_uint(pid);
        icda_write(" exit=");
        write_uint(code);
        icda_write("\n");
        return 1;
    }
    return 0;
}

static void shell_dispatch(char *line) {
    char *arg = 0;
    while (*line == ' ' || *line == '\t') line++;
    for (uint64_t i = 0; line[i]; i++) {
        if (line[i] == ' ' || line[i] == '\t') {
            line[i] = 0;
            arg = &line[i + 1];
            while (*arg == ' ' || *arg == '\t') arg++;
            break;
        }
    }
    if (*line == 0) return;
    if (str_eq(line, "help")) { shell_help(); return; }
    if (str_eq(line, "clear")) { icda_clear(); return; }
    if (str_eq(line, "pid")) { icda_write("pid="); write_uint(icda_get_pid()); icda_write("\n"); return; }
    if (str_eq(line, "ps")) { shell_ps(); return; }
    if (str_eq(line, "pwd")) { shell_pwd(); return; }
    if (str_eq(line, "cd")) { if ((long)icda_chdir((arg && *arg) ? arg : "/") < 0) icda_write("cd failed\n"); return; }
    if (str_eq(line, "ls")) { shell_ls(arg); return; }
    if (str_eq(line, "cat")) { shell_cat(arg); return; }
    if (str_eq(line, "mkdir")) { shell_mkdir(arg); return; }
    if (str_eq(line, "touch")) { shell_touch(arg); return; }
    if (str_eq(line, "write")) { shell_write_file(arg); return; }
    if (str_eq(line, "stat")) { shell_stat(arg); return; }
    if (str_eq(line, "run")) { shell_run_path(arg); return; }
    if (str_eq(line, "spawn")) { shell_spawn_path(arg); return; }
    if (str_eq(line, "wait")) { shell_wait_pid(arg); return; }
    if (str_eq(line, "exit")) icda_exit(0);
    if (!shell_try_exec_command(line)) {
        icda_write("unknown command: ");
        icda_write(line);
        icda_write("\n");
    }
}

uint64_t shell_main(void) {
    char line[SHELL_LINE_CAP];
    icda_clear();
    icda_write("icda user shell\n");
    icda_write("type 'help' for commands\n\n");
    for (;;) {
        uint64_t len = 0;
        print_prompt();
        for (;;) {
            long ch = icda_read_char();
            if (ch < 0) continue;
            shell_hide_cursor();
            if (ch == '\r' || ch == '\n') {
                icda_write("\n");
                line[len] = 0;
                break;
            }
            if (ch == '\b') {
                if (len > 0) {
                    len--;
                    line[len] = 0;
                    icda_backspace();
                }
                shell_show_cursor();
                continue;
            }
            if (ch == '\t') ch = ' ';
            if (ch < 32 || ch > 126) {
                shell_show_cursor();
                continue;
            }
            if (len + 1 >= sizeof(line)) {
                shell_show_cursor();
                continue;
            }
            line[len++] = (char)ch;
            line[len] = 0;
            write_char((char)ch);
            shell_show_cursor();
        }
        shell_dispatch(line);
        shell_cursor_visible = 0;
    }
}
