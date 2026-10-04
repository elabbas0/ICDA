#include "icda_sys.h"
#include "settings_store.h"

#include <stdint.h>

#define SHELL_LINE_CAP 128
#define SHELL_BUF_CAP 1024
#define SHELL_HISTORY_CAP 16
#define SHELL_EDIT_BUF_CAP 4096
#define SHELL_EDIT_VIEW_ROWS 18
#define SHELL_EDIT_VIEW_COLS 68
#define SHELL_EDIT_REQUEST_PATH "/home/.edit.request"
#define SHELL_CURL_REQUEST_PATH "/home/.curl.request"
#define SHELL_SCRIPT_CAP 16384


static char shell_history[SHELL_HISTORY_CAP][SHELL_LINE_CAP];
static uint64_t shell_history_count = 0;
enum {
    KEY_SPECIAL_BASE = 256,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_DELETE
};

static void shell_storage(void);
static void shell_dispatch(char *line);

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

static void copy_text(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!cap) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int str_prefix(const char *text, const char *prefix) {
    uint64_t i = 0;
    while (prefix[i]) {
        if (text[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
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

static int parse_uint64(const char *text, uint64_t *out) {
    uint64_t value = 0;
    uint64_t i = 0;

    if (!text || !*text || !out) return 0;
    while (text[i]) {
        if (text[i] < '0' || text[i] > '9') return 0;
        value = value * 10 + (uint64_t)(text[i] - '0');
        i++;
    }
    *out = value;
    return 1;
}

static void shell_play_wav_path(const char *path) {
    char fallback[SHELL_LINE_CAP];
    char cwd[80];
    const char *resolved = path;
    icda_settings_t audio_opt;

    if (!path || !*path) {
        icda_write("usage: play <path>\n");
        return;
    }

    
    icda_settings_load(&audio_opt);
    if (!audio_opt.audio) {
        icda_write("audio disabled (enable in Settings)\n");
        return;
    }

    if (!str_has_slash(path)) {
        copy_text(fallback, "/usr/share/audio/", sizeof(fallback));
        append_text(fallback, path, sizeof(fallback));
        resolved = fallback;
    } else if (path[0] != '/') {
        if ((long)icda_getcwd(cwd, sizeof(cwd)) < 0) {
            icda_write("play failed: ");
            icda_write(path);
            icda_write("\n");
            return;
        }
        copy_text(fallback, cwd, sizeof(fallback));
        if (!str_eq(fallback, "/") && fallback[str_len(fallback) - 1] != '/') {
            append_text(fallback, "/", sizeof(fallback));
        }
        append_text(fallback, path, sizeof(fallback));
        resolved = fallback;
    }

    if (str_len(resolved) == 0 || str_len(resolved) + 1 >= SHELL_LINE_CAP) {
        icda_write("play failed: ");
        icda_write(path);
        icda_write("\n");
        return;
    }

    if ((long)icda_play_audio_file(resolved) < 0) {
        icda_write("play failed: ");
        icda_write(resolved);
        icda_write("\n");
    }
}

static int shell_resolve_path(const char *path, char *resolved, uint64_t cap) {
    char cwd[80];

    if (!path || !*path || !resolved || cap == 0) return -1;
    if (path[0] == '/') {
        copy_text(resolved, path, cap);
        return 0;
    }
    if ((long)icda_getcwd(cwd, sizeof(cwd)) < 0) return -1;
    copy_text(resolved, cwd, cap);
    if (!str_eq(resolved, "/") && resolved[str_len(resolved) - 1] != '/') append_text(resolved, "/", cap);
    append_text(resolved, path, cap);
    return 0;
}

static void shell_launch_foreground_request(const char *request_path, const char *payload, const char *app_path, const char *usage_text, int clear_screen) {
    uint64_t pid;

    if (!payload || !*payload) {
        icda_write(usage_text);
        return;
    }
    if ((long)icda_write_file(request_path, payload, str_len(payload)) < 0) {
        icda_write("launch failed\n");
        return;
    }
    if (clear_screen) {
        icda_clear();
    }
    pid = icda_spawn(app_path);
    if ((long)pid < 0) {
        if (clear_screen) {
            icda_clear();
        }
        icda_write("launch failed\n");
        return;
    }
    (void)icda_waitpid(pid);
    if (clear_screen) {
        icda_clear();
    }
}

static void shell_stop_audio(void) {
    (void)icda_stop_audio();
}

static void shell_curl(const char *arg) {
    char url[256];
    char out_path[160];
    char request[384];
    char resolved[160];
    uint64_t i = 0;
    uint64_t j = 0;

    if (!arg || !*arg) {
        icda_write("usage: curl <http://host[:port]/path> <out-path>\n");
        return;
    }

    while (arg[i] && arg[i] != ' ' && arg[i] != '\t' && i + 1 < sizeof(url)) {
        url[i] = arg[i];
        i++;
    }
    url[i] = 0;
    while (arg[i] == ' ' || arg[i] == '\t') i++;
    while (arg[i] && j + 1 < sizeof(out_path)) {
        out_path[j++] = arg[i++];
    }
    out_path[j] = 0;

    request[0] = 0;
    append_text(request, url, sizeof(request));
    if (out_path[0]) {
        if (shell_resolve_path(out_path, resolved, sizeof(resolved)) != 0) {
            icda_write("curl: bad output path\n");
            return;
        }
        append_text(request, "\n", sizeof(request));
        append_text(request, resolved, sizeof(request));
    }
    shell_launch_foreground_request(SHELL_CURL_REQUEST_PATH, request, "/apps/curl.app", "usage: curl <http://host[:port]/path> <out-path>\n", 0);
}

static void shell_history_add(const char *line) {
    if (!line || !*line) return;
    if (shell_history_count && str_eq(shell_history[(shell_history_count - 1) % SHELL_HISTORY_CAP], line)) {
        return;
    }
    copy_text(shell_history[shell_history_count % SHELL_HISTORY_CAP], line, SHELL_LINE_CAP);
    shell_history_count++;
}

static int shell_relative = 0;

static void shell_render_line(const char *line, uint64_t len, uint64_t cursor, uint64_t prompt_x, uint64_t prompt_y, uint64_t *shown_len) {
    uint64_t i;
    uint64_t old_len = shown_len ? *shown_len : 0;
    uint64_t pad = old_len > len ? old_len - len : 0;

    if (shell_relative) {
        icda_write("\x1b" "8");
        if (len) icda_write(line);
        icda_write("\x1b[K\x1b" "8");
        if (cursor) {
            icda_write("\x1b[");
            write_uint(cursor);
            icda_write("C");
        }
        if (shown_len) *shown_len = len;
        return;
    }
    icda_set_cursor(prompt_x, prompt_y);
    if (len) {
        icda_write(line);
    }
    for (i = 0; i < pad; i++) {
        icda_write(" ");
    }
    icda_set_cursor(prompt_x + cursor, prompt_y);
    if (shown_len) *shown_len = len;
}

static void shell_cursor_show(int *visible) {
    if (!*visible) {
        *visible = 1;
    }
}

static void shell_cursor_hide(int *visible) {
    if (*visible) {
        *visible = 0;
    }
}

static long shell_wait_key_byte(uint64_t timeout_ticks) {
    return icda_read_char_timeout(timeout_ticks);
}

static uint64_t shell_collect_matches(const char *dir, const char *prefix, char matches[][SHELL_LINE_CAP], uint64_t max_matches) {
    char buf[SHELL_BUF_CAP];
    uint64_t count = 0;
    uint64_t start = 0;

    if ((long)icda_list_dir(dir, buf, sizeof(buf)) < 0) {
        return 0;
    }

    while (buf[start] && count < max_matches) {
        char entry[SHELL_LINE_CAP];
        uint64_t out = 0;
        while (buf[start] && buf[start] != '\n' && out + 1 < sizeof(entry)) {
            entry[out++] = buf[start++];
        }
        if (buf[start] == '\n') start++;
        entry[out] = 0;
        if (out && entry[out - 1] == '/') {
            entry[out - 1] = 0;
        }
        if (str_prefix(entry, prefix)) {
            copy_text(matches[count++], entry, SHELL_LINE_CAP);
        }
    }
    return count;
}

static int shell_autocomplete(char *line, uint64_t cap) {
    char matches[32][SHELL_LINE_CAP];
    char token[SHELL_LINE_CAP];
    uint64_t len = str_len(line);
    uint64_t token_start = len;
    uint64_t match_count = 0;

    while (token_start > 0 && line[token_start - 1] != ' ' && line[token_start - 1] != '\t') {
        token_start--;
    }
    copy_text(token, &line[token_start], sizeof(token));

    if (token_start == 0 && !str_has_slash(token)) {
        static const char *builtins[] = {
            "help","clear","pwd","cd","ls","cat","mkdir","touch","write","stat","install","sync","storage","mount","play","stop",
            "edit","diskman","curl","run","exit"
        };
        for (uint64_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]) && match_count < 32; i++) {
            if (str_prefix(builtins[i], token)) {
                copy_text(matches[match_count++], builtins[i], SHELL_LINE_CAP);
            }
        }
        match_count += shell_collect_matches("/apps", token, &matches[match_count], 32 - match_count);
        match_count += shell_collect_matches("/bin", token, &matches[match_count], 32 - match_count);
    } else {
        char dir[SHELL_LINE_CAP];
        char prefix[SHELL_LINE_CAP];
        uint64_t slash = 0;
        for (uint64_t i = 0; token[i]; i++) {
            if (token[i] == '/') slash = i + 1;
        }
        if (slash) {
            uint64_t i = 0;
            for (; i < slash && i + 1 < sizeof(dir); i++) dir[i] = token[i];
            dir[i] = 0;
            copy_text(prefix, &token[slash], sizeof(prefix));
            if (dir[0] == 0) copy_text(dir, "/", sizeof(dir));
        } else {
            copy_text(dir, ".", sizeof(dir));
            copy_text(prefix, token, sizeof(prefix));
        }
        match_count = shell_collect_matches(dir, prefix, matches, 32);
        if (match_count == 1 && slash) {
            char full[SHELL_LINE_CAP];
            copy_text(full, dir, sizeof(full));
            if (!str_eq(dir, "/") && full[str_len(full) - 1] != '/') append_text(full, "/", sizeof(full));
            append_text(full, matches[0], sizeof(full));
            copy_text(matches[0], full, SHELL_LINE_CAP);
        }
    }

    if (match_count == 0) {
        return 0;
    }

    copy_text(&line[token_start], matches[0], cap - token_start);
    return 1;
}

static int shell_read_line(char *line, uint64_t cap) {
    uint64_t len = 0;
    uint64_t cursor = 0;
    uint64_t shown_len = 0;
    uint64_t history_cursor = shell_history_count;
    uint64_t last_blink = icda_ticks();
    uint64_t idle_since = last_blink;
    int cursor_visible = 0;
    uint64_t prompt_x = 0;
    uint64_t prompt_y = 0;

    if (!line || cap == 0) return -1;
    line[0] = 0;
    shell_relative = (long)icda_console_cursor(&prompt_x, &prompt_y) < 0;
    if (shell_relative) icda_write("\x1b" "7");
    shell_cursor_show(&cursor_visible);

    for (;;) {
        long c = shell_wait_key_byte(1);
        if (c < 0) {
            uint64_t now = icda_ticks();
            if (now - idle_since < 40) {
                shell_cursor_show(&cursor_visible);
            } else if (now - last_blink >= 50) {
                if (cursor_visible) {
                    shell_cursor_hide(&cursor_visible);
                } else {
                    shell_cursor_show(&cursor_visible);
                }
                last_blink = now;
            }
            continue;
        }

        if (c == 27) {
            long c1 = shell_wait_key_byte(2);
            if (c1 == '[') {
                switch (shell_wait_key_byte(2)) {
                    case 'A': c = KEY_UP; break;
                    case 'B': c = KEY_DOWN; break;
                    case 'C': c = KEY_RIGHT; break;
                    case 'D': c = KEY_LEFT; break;
                    case '3':
                        if (shell_wait_key_byte(2) == '~') c = KEY_DELETE;
                        break;
                    default: break;
                }
            }
        }

        shell_cursor_hide(&cursor_visible);
        idle_since = icda_ticks();
        last_blink = idle_since;

        if (c == '\r' || c == '\n') {
            icda_write("\n");
            line[len] = 0;
            if (len) shell_history_add(line);
            return (int)len;
        }

        if (c == '\t') {
            if (shell_autocomplete(line, cap)) {
                len = str_len(line);
                cursor = len;
                shell_render_line(line, len, cursor, prompt_x, prompt_y, &shown_len);
            }
            shell_cursor_show(&cursor_visible);
            continue;
        }

        if (c == '\b' || c == 127) {
            if (cursor > 0) {
                for (uint64_t i = cursor - 1; i < len; i++) {
                    line[i] = line[i + 1];
                }
                cursor--;
                len--;
                line[len] = 0;
                shell_render_line(line, len, cursor, prompt_x, prompt_y, &shown_len);
            }
            shell_cursor_show(&cursor_visible);
            continue;
        }

        if (c == KEY_UP) {
            if (shell_history_count) {
                if (history_cursor > 0) history_cursor--;
                copy_text(line, shell_history[history_cursor % SHELL_HISTORY_CAP], cap);
                len = str_len(line);
                cursor = len;
                shell_render_line(line, len, cursor, prompt_x, prompt_y, &shown_len);
            }
            shell_cursor_show(&cursor_visible);
            continue;
        }

        if (c == KEY_DOWN) {
            if (history_cursor < shell_history_count) history_cursor++;
            if (history_cursor == shell_history_count) {
                line[0] = 0;
            } else {
                copy_text(line, shell_history[history_cursor % SHELL_HISTORY_CAP], cap);
            }
            len = str_len(line);
            cursor = len;
            shell_render_line(line, len, cursor, prompt_x, prompt_y, &shown_len);
            shell_cursor_show(&cursor_visible);
            continue;
        }

        if (c == KEY_LEFT) {
            if (cursor > 0) cursor--;
            shell_render_line(line, len, cursor, prompt_x, prompt_y, &shown_len);
            shell_cursor_show(&cursor_visible);
            continue;
        }

        if (c == KEY_RIGHT) {
            if (cursor < len) cursor++;
            shell_render_line(line, len, cursor, prompt_x, prompt_y, &shown_len);
            shell_cursor_show(&cursor_visible);
            continue;
        }

        if (c == KEY_DELETE) {
            if (cursor < len) {
                for (uint64_t i = cursor; i < len; i++) {
                    line[i] = line[i + 1];
                }
                len--;
                line[len] = 0;
                shell_render_line(line, len, cursor, prompt_x, prompt_y, &shown_len);
            }
            shell_cursor_show(&cursor_visible);
            continue;
        }

        if (c >= 32 && c <= 126 && len + 1 < cap) {
            for (uint64_t i = len; i > cursor; i--) {
                line[i] = line[i - 1];
            }
            line[cursor] = (char)c;
            len++;
            cursor++;
            line[len] = 0;
            shell_render_line(line, len, cursor, prompt_x, prompt_y, &shown_len);
            shell_cursor_show(&cursor_visible);
        }
    }
}

static void shell_edit(const char *path) {
    char resolved[SHELL_LINE_CAP];

    if (!path || !*path) {
        icda_write("usage: edit <path>\n");
        return;
    }
    if (shell_resolve_path(path, resolved, sizeof(resolved)) != 0) {
        icda_write("edit failed\n");
        return;
    }
    shell_launch_foreground_request(SHELL_EDIT_REQUEST_PATH, resolved, "/apps/editor.app", "usage: edit <path>\n", 1);
}

static void print_prompt(void) {
    char cwd[80];
    if ((long)icda_getcwd(cwd, sizeof(cwd)) < 0) {
        icda_write("icda:/ ");
        return;
    }
    {
        uint64_t x, y;
        if ((long)icda_console_cursor(&x, &y) < 0) {
            icda_write("\x1b[1;32micda\x1b[0m:\x1b[1;34m");
            icda_write(cwd);
            icda_write("\x1b[0m$ ");
            return;
        }
    }
    icda_write("icda:");
    icda_write(cwd);
    icda_write(" ");
}

static void shell_help(void) {
    icda_write("commands: help clear pwd cd ls cat echo mkdir touch write stat install sync storage mount play stop edit diskman curl run exit\n");
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

static void shell_echo(const char *text) {
    if (text && *text) icda_write(text);
    icda_write("\n");
}

static int shell_spawn_and_wait(const char *path, const char *args) {
    uint64_t pid;
    uint64_t code;
    pid = (args && *args) ? icda_spawn_args(path, args) : icda_spawn(path);
    if ((long)pid < 0) {
        return -1;
    }
    code = icda_waitpid(pid);
    if ((long)code < 0) {
        return -2;
    }
    icda_write("pid=");
    write_uint(pid);
    icda_write(" ");
    icda_write("exit=");
    write_uint(code);
    icda_write("\n");
    return 0;
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

static void shell_run_path(const char *path) {
    char launch_path[160];
    const char *args = 0;
    uint64_t i = 0;

    if (!path || !*path) {
        icda_write("usage: run <path> [args]\n");
        return;
    }

    while (path[i] && path[i] != ' ' && path[i] != '\t' && i + 1 < sizeof(launch_path)) {
        launch_path[i] = path[i];
        i++;
    }
    launch_path[i] = 0;
    args = &path[i];
    while (*args == ' ' || *args == '\t') args++;
    if (!*args) args = 0;

    if (shell_spawn_and_wait(launch_path, args) == 0) {
        return;
    }
    icda_write("run failed: ");
    icda_write(launch_path);
    icda_write("\n");
}

static int shell_run_script(const char *path) {
    static char script[SHELL_SCRIPT_CAP];
    long ret;
    uint64_t i = 0;
    uint64_t line_start = 0;

    if (!path || !*path) return -1;
    ret = (long)icda_read_file(path, script, sizeof(script));
    if (ret < 0) {
        icda_write("script read failed: ");
        icda_write(path);
        icda_write("\n");
        return -1;
    }
    if (ret == 0) {
        return 0;
    }
    if (script[0] == '#' && script[1] == '!') {
        while (i < (uint64_t)ret && script[i] != '\n') i++;
        if (i < (uint64_t)ret) i++;
        line_start = i;
    }

    for (; i <= (uint64_t)ret; i++) {
        if (i == (uint64_t)ret || script[i] == '\n' || script[i] == '\r') {
            char *line = &script[line_start];
            script[i] = 0;
            while (*line == ' ' || *line == '\t') line++;
            if (*line && *line != '#') {
                shell_dispatch(line);
            }
            while (i + 1 < (uint64_t)ret && (script[i + 1] == '\n' || script[i + 1] == '\r')) i++;
            line_start = i + 1;
        }
    }
    return 0;
}

static void shell_sync(void) {
    if ((long)icda_sync() < 0) {
        icda_write("sync failed\n");
        return;
    }
    icda_write("synced\n");
}

static void shell_install_target(const char *arg) {
    uint64_t files = 0;
    uint64_t bytes = 0;
    uint64_t device = 0;
    long rc;
    icda_install_plan_t plan;
    char buf[64];

    if (!arg || !*arg) {
        shell_storage();
        icda_write("efi partition index: ");
        if ((long)icda_read_line(buf, sizeof(buf)) < 0 || !parse_uint64(buf, &plan.efi_partition)) {
            icda_write("install cancelled\n");
            return;
        }
        icda_write("root partition index: ");
        if ((long)icda_read_line(buf, sizeof(buf)) < 0 || !parse_uint64(buf, &plan.root_partition)) {
            icda_write("install cancelled\n");
            return;
        }
        icda_write("swap partition index (-1 for none): ");
        if ((long)icda_read_line(buf, sizeof(buf)) < 0) {
            icda_write("install cancelled\n");
            return;
        }
        if (buf[0] == '-' && buf[1] == '1' && buf[2] == 0) {
            plan.swap_partition = -1;
        } else if (!parse_uint64(buf, &device)) {
            icda_write("install cancelled\n");
            return;
        } else {
            plan.swap_partition = (int64_t)device;
        }
        icda_clear();
        rc = (long)icda_install_partitions(&plan, &files, &bytes);
        icda_clear();
        if (rc < 0) {
            icda_write("install failed\n");
            icda_write("error: ");
            write_uint((uint64_t)(-rc));
            icda_write("\n");
            return;
        }
        icda_write("installed bootable system to selected partitions: ");
        write_uint(files);
        icda_write(" files, ");
        write_uint(bytes);
        icda_write(" bytes bundled\n");
        icda_write("you can now try booting from that disk without the iso/usb\n");
        return;
    }
    if (!parse_uint64(arg, &device)) {
        icda_write("usage: install [device-index]\n");
        return;
    }
    icda_clear();
    rc = (long)icda_install_device(device, &files, &bytes);
    icda_clear();
    if (rc < 0) {
        icda_write("install failed\n");
        icda_write("error: ");
        write_uint((uint64_t)(-rc));
        icda_write("\n");
        return;
    }
    icda_write("installed bootable system to device ");
    write_uint(device);
    icda_write(": ");
    write_uint(files);
    icda_write(" files, ");
    write_uint(bytes);
    icda_write(" bytes persisted\n");
    icda_write("you can now try booting from that disk without the iso/usb\n");
}

static void shell_storage(void) {
    char buf[SHELL_BUF_CAP];
    long ret = (long)icda_storage_info(buf, sizeof(buf));
    if (ret < 0) {
        icda_write("storage query failed\n");
        return;
    }
    icda_write(buf);
}

static void shell_diskman(void) {
    uint64_t pid = icda_spawn("/apps/diskman.app");
    if ((long)pid < 0) {
        icda_write("diskman failed\n");
        return;
    }
    (void)icda_waitpid(pid);
}

static void shell_mount(const char *arg) {
    char part_text[32];
    uint64_t part = 0;
    const char *mount_path;
    uint64_t i = 0;

    if (!arg || !*arg) {
        icda_write("usage: mount <partition-index> <path>\n");
        return;
    }

    while (arg[i] && arg[i] != ' ' && arg[i] != '\t' && i + 1 < sizeof(part_text)) {
        part_text[i] = arg[i];
        i++;
    }
    part_text[i] = 0;
    mount_path = &arg[i];
    while (*mount_path == ' ' || *mount_path == '\t') {
        mount_path++;
    }

    if (!part_text[0] || !*mount_path || !parse_uint64(part_text, &part)) {
        icda_write("usage: mount <partition-index> <path>\n");
        return;
    }

    if ((long)icda_mount(part, mount_path) < 0) {
        icda_write("mount failed (no such detected partition or unsupported fs)\n");
        return;
    }

    icda_write("mounted partition ");
    write_uint(part);
    icda_write(" at ");
    icda_write(mount_path);
    icda_write("\n");
}

static int shell_try_exec_command(const char *cmd, const char *arg) {
    char path[160];
    if (shell_spawn_and_wait(cmd, arg) == 0) {
        return 1;
    }
    if (str_has_slash(cmd)) return 0;

    path[0] = 0;
    append_text(path, "/apps/", sizeof(path));
    append_text(path, cmd, sizeof(path));
    if (shell_spawn_and_wait(path, arg) == 0) {
        return 1;
    }

    path[0] = 0;
    append_text(path, "/bin/", sizeof(path));
    append_text(path, cmd, sizeof(path));
    if (shell_spawn_and_wait(path, arg) == 0) {
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
    if (str_eq(line, "pwd")) { shell_pwd(); return; }
    if (str_eq(line, "cd")) { if ((long)icda_chdir((arg && *arg) ? arg : "/") < 0) icda_write("cd failed\n"); return; }
    if (str_eq(line, "ls")) { shell_ls(arg); return; }
    if (str_eq(line, "cat")) { shell_cat(arg); return; }
    if (str_eq(line, "echo")) { shell_echo(arg); return; }
    if (str_eq(line, "mkdir")) { shell_mkdir(arg); return; }
    if (str_eq(line, "touch")) { shell_touch(arg); return; }
    if (str_eq(line, "write")) { shell_write_file(arg); return; }
    if (str_eq(line, "stat")) { shell_stat(arg); return; }
    if (str_eq(line, "install")) { shell_install_target(arg); return; }
    if (str_eq(line, "sync")) { shell_sync(); return; }
    if (str_eq(line, "storage")) { shell_storage(); return; }
    if (str_eq(line, "mount")) { shell_mount(arg); return; }
    if (str_eq(line, "play")) { shell_play_wav_path(arg); return; }
    if (str_eq(line, "stop")) { shell_stop_audio(); return; }
    if (str_eq(line, "edit")) { shell_edit(arg); return; }
    if (str_eq(line, "diskman")) { shell_diskman(); return; }
    if (str_eq(line, "curl")) { shell_curl(arg); return; }
    if (str_eq(line, "run")) { shell_run_path(arg); return; }
    if (str_eq(line, "exit")) icda_exit(0);
    if (!shell_try_exec_command(line, arg)) {
        icda_write("unknown command: ");
        icda_write(line);
        icda_write("\n");
    }
}

uint64_t shell_main(uint64_t argc, char **argv) {
    char line[SHELL_LINE_CAP];
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        return shell_run_script(argv[1]) == 0 ? 0 : 1;
    }
    (void)icda_chdir("/home");
    icda_clear();
    icda_write("icda user shell\n\n");
    for (;;) {
        print_prompt();
        if (shell_read_line(line, sizeof(line)) < 0) {
            continue;
        }
        shell_dispatch(line);
    }
}
