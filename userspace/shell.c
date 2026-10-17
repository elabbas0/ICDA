#include "icda_sys.h"
#include "settings_store.h"

#include <stdint.h>

#define SHELL_LINE_CAP 2048
#define SHELL_BUF_CAP 1024
#define SHELL_HISTORY_CAP 16
#define SHELL_EDIT_BUF_CAP 4096
#define SHELL_EDIT_VIEW_ROWS 18
#define SHELL_EDIT_VIEW_COLS 68
#define SHELL_EDIT_REQUEST_PATH "/home/.edit.request"
#define SHELL_CURL_REQUEST_PATH "/home/.curl.request"
#define SHELL_SCRIPT_CAP 16384


/* "command > file", "command >> file": while a line's output is captured,
 * what the built-in commands print goes here, and into the file after */
#define SHELL_CAPTURE_CAP 65536
static char shell_capture_buf[SHELL_CAPTURE_CAP];
static uint64_t shell_capture_len;
static int shell_capturing;

/* "a && b" runs b only if a worked: a command failed when it said so, the
 * way every built-in reports a problem */
static int shell_failed;

static int shell_has(const char *s, const char *sub) {
    for (; *s; s++) {
        const char *a = s, *b = sub;
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static void shell_note_failure(const char *t) {
    const char *u = "usage:", *n = "unknown command";
    int i = 0;
    while (u[i] && t[i] == u[i]) i++;
    if (!u[i]) { shell_failed = 1; return; }
    i = 0;
    while (n[i] && t[i] == n[i]) i++;
    if (!n[i] || shell_has(t, " failed") || shell_has(t, "cannot ") || shell_has(t, "could not")) shell_failed = 1;
}

static uint64_t shell_out(const char *text) {
    shell_note_failure(text);
    if (!shell_capturing) return icda_write(text);
    for (; *text && shell_capture_len + 1 < SHELL_CAPTURE_CAP; text++) shell_capture_buf[shell_capture_len++] = *text;
    return 0;
}
#define icda_write shell_out

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
            "help","clear","pwd","cd","ls","cat","grep","head","tail","wc","sort","mkdir","rm","touch","write","stat","install","sync","storage","mount","play","stop",
            "edit","diskman","curl","notify","run","exit"
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
            /* Interactive Linux children can leave bracketed paste enabled
             * after interruption. This shell reads plain input at its prompt. */
            icda_write("\x1b[?2004l\x1b[1;32micda\x1b[0m:\x1b[1;34m");
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
    icda_write("commands: help clear pwd cd ls cat echo grep head tail wc sort mkdir touch write stat install sync\n"
               "          storage mount play stop edit diskman curl wifi webkit codex run exit\n"
               "  cmd > file, cmd >> file   output into a file (replace, add to)\n"
               "  a | b                     a's output is b's input (cat, grep, head, tail, wc, sort)\n"
               "  a ; b, a && b             one after the other; with &&, b only if a worked\n");
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

/* ---- filters: a file, or what the command before "|" printed ---------------------- */

static const char *shell_pipe_in;          /* the previous command's output, in a pipeline */
static uint64_t shell_pipe_len;
static char shell_text[SHELL_CAPTURE_CAP];

/* the text a filter works on: path's contents, or the piped input; 0 if neither */
static const char *shell_input(const char *path, const char *cmd) {
    if (path && *path) {
        long ret = (long)icda_read_file(path, shell_text, sizeof(shell_text) - 1);
        if (ret < 0) {
            icda_write(cmd);
            icda_write(": cannot read ");
            icda_write(path);
            icda_write("\n");
            return 0;
        }
        shell_text[ret] = 0;
        return shell_text;
    }
    if (shell_pipe_in) return shell_pipe_in;
    icda_write("usage: ");
    icda_write(cmd);
    icda_write(" <path>, or after |\n");
    return 0;
}

/* each line of text to emit(line, len) */
static void shell_lines(const char *text, void (*emit)(const char *, uint64_t, void *), void *ctx) {
    while (*text) {
        uint64_t n = 0;
        while (text[n] && text[n] != '\n') n++;
        emit(text, n, ctx);
        text += n;
        if (*text == '\n') text++;
    }
}

static void shell_put_line(const char *s, uint64_t n) {
    char line[SHELL_LINE_CAP];
    if (n >= sizeof(line)) n = sizeof(line) - 1;
    for (uint64_t i = 0; i < n; i++) line[i] = s[i];
    line[n] = '\n';
    line[n + 1 < sizeof(line) ? n + 1 : n] = 0;
    icda_write(line);
}

static void shell_cat(const char *path) {
    const char *text = shell_input(path, "cat");
    uint64_t n;
    if (!text) return;
    icda_write(text);
    n = 0;
    while (text[n]) n++;
    if (n && text[n - 1] != '\n') icda_write("\n");
}

static char shell_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

typedef struct { const char *pat; int icase, invert; uint64_t hits; } grep_ctx_t;

static void grep_line(const char *s, uint64_t n, void *p) {
    grep_ctx_t *g = (grep_ctx_t *)p;
    int found = 0;
    for (uint64_t i = 0; i < n && !found; i++) {
        uint64_t k = 0;
        while (g->pat[k] && i + k < n &&
               (g->icase ? shell_lower(s[i + k]) == shell_lower(g->pat[k]) : s[i + k] == g->pat[k])) k++;
        if (!g->pat[k]) found = 1;
    }
    if (!g->pat[0]) found = 1;
    if (found != g->invert) { shell_put_line(s, n); g->hits++; }
}

/* grep [-i] [-v] text [path]: the lines with text in them */
static void shell_grep(char *arg) {
    grep_ctx_t g = { 0, 0, 0, 0 };
    char *path = 0;
    const char *text;
    while (arg && arg[0] == '-' && (arg[1] == 'i' || arg[1] == 'v') && (arg[2] == ' ' || !arg[2])) {
        if (arg[1] == 'i') g.icase = 1; else g.invert = 1;
        arg += 2;
        while (*arg == ' ') arg++;
    }
    if (!arg || !*arg) { icda_write("usage: grep [-i] [-v] <text> [path]\n"); return; }
    if (*arg == '"' || *arg == '\'') {                 /* "a quoted pattern" */
        char q = *arg++;
        g.pat = arg;
        while (*arg && *arg != q) arg++;
        if (*arg) *arg++ = 0;
    } else {
        g.pat = arg;
        while (*arg && *arg != ' ') arg++;
        if (*arg) *arg++ = 0;
    }
    while (*arg == ' ') arg++;
    path = *arg ? arg : 0;
    if (!(text = shell_input(path, "grep"))) return;
    shell_lines(text, grep_line, &g);
}

typedef struct { uint64_t n, at, total; } count_ctx_t;
static void count_line(const char *s, uint64_t n, void *p) { (void)s; (void)n; ((count_ctx_t *)p)->total++; }
static void head_line(const char *s, uint64_t n, void *p) {
    count_ctx_t *c = (count_ctx_t *)p;
    if (c->at++ < c->n) shell_put_line(s, n);
}
static void tail_line(const char *s, uint64_t n, void *p) {
    count_ctx_t *c = (count_ctx_t *)p;
    if (c->at++ >= c->total - (c->total < c->n ? c->total : c->n)) shell_put_line(s, n);
}

/* head/tail [-n N] [path]: the first or last N lines (10) */
static void shell_head_tail(char *arg, int tail) {
    count_ctx_t c = { 10, 0, 0 };
    const char *text;
    if (arg && arg[0] == '-' && arg[1] == 'n') {
        arg += 2;
        while (*arg == ' ') arg++;
        c.n = 0;
        while (*arg >= '0' && *arg <= '9') c.n = c.n * 10 + (uint64_t)(*arg++ - '0');
        while (*arg == ' ') arg++;
    }
    if (!(text = shell_input(arg && *arg ? arg : 0, tail ? "tail" : "head"))) return;
    if (tail) shell_lines(text, count_line, &c);
    shell_lines(text, tail ? tail_line : head_line, &c);
}

/* wc [path]: lines, words, bytes */
static void shell_wc(const char *path) {
    const char *text = shell_input(path, "wc");
    uint64_t lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    if (!text) return;
    for (const char *p = text; *p; p++, bytes++) {
        if (*p == '\n') lines++;
        if (*p == ' ' || *p == '\n' || *p == '\t') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    write_uint(lines); icda_write(" lines, ");
    write_uint(words); icda_write(" words, ");
    write_uint(bytes); icda_write(" bytes\n");
}

/* sort [path]: the lines in order */
static void shell_sort(const char *path) {
    static const char *line[2048];
    static uint64_t len[2048];
    const char *text = shell_input(path, "sort");
    uint64_t n = 0;
    if (!text) return;
    while (*text && n < 2048) {
        uint64_t k = 0;
        while (text[k] && text[k] != '\n') k++;
        line[n] = text;
        len[n++] = k;
        text += k;
        if (*text == '\n') text++;
    }
    for (uint64_t i = 1; i < n; i++) {                 /* insertion sort: a few thousand lines at most */
        const char *l = line[i];
        uint64_t ln = len[i], j = i;
        while (j > 0) {
            const char *a = line[j - 1];
            uint64_t an = len[j - 1], k = 0;
            while (k < an && k < ln && a[k] == l[k]) k++;
            /* a after l: a longer with l as its start, or larger where they differ */
            if (!(k < an && (k == ln || (unsigned char)a[k] > (unsigned char)l[k]))) break;
            line[j] = line[j - 1];
            len[j] = len[j - 1];
            j--;
        }
        line[j] = l;
        len[j] = ln;
    }
    for (uint64_t i = 0; i < n; i++) shell_put_line(line[i], len[i]);
}

/* echo "text" prints text: one pair of quotes around it is taken off */
static void shell_echo(const char *text) {
    char buf[SHELL_LINE_CAP];
    uint64_t n = 0;
    if (text) while (text[n] && n + 1 < sizeof(buf)) { buf[n] = text[n]; n++; }
    buf[n] = 0;
    if (n >= 2 && (buf[0] == '"' || buf[0] == '\'') && buf[n - 1] == buf[0]) {
        buf[n - 1] = 0;
        icda_write(buf + 1);
    } else if (n) {
        icda_write(buf);
    }
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
    if (code != 0) shell_failed = 1;          /* for "a && b" */
    icda_write("pid=");
    write_uint(pid);
    icda_write(" ");
    icda_write("exit=");
    write_uint(code);
    icda_write("\n");
    return 0;
}

static void shell_rm(const char *path) {
    if (!path || !*path) {
        icda_write("usage: rm <path>\n");
        return;
    }
    if (icda_remove(path) < 0) icda_write("rm failed\n");
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

    if (*arg == '"') {
        arg++;
        text = (char *)arg;
        while (*text && *text != '"') text++;
        if (!*text) {
            icda_write("usage: write \"<path>\" <text>\n");
            return;
        }
    } else {
        text = (char *)arg;
        while (*text && *text != ' ' && *text != '\t') text++;
        if (!*text) {
            icda_write("usage: write <path> <text>\n");
            return;
        }
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

/* ---- wifi: front end for /dev/wifi ------------------------------------ */

#define WIFI_DEV "/dev/wifi"
static char wifi_buf[16384];

static int wifi_cmd(const char *cmd) {
    return (long)icda_write_file(WIFI_DEV, cmd, str_len(cmd)) < 0 ? -1 : 0;
}

static void wifi_read(void) {
    if ((long)icda_read_file(WIFI_DEV, wifi_buf, sizeof(wifi_buf)) < 0) wifi_buf[0] = 0;
}

/* Value of "key: value" in wifi_buf (empty if absent). */
static void wifi_field(const char *key, char *out, uint64_t cap) {
    uint64_t klen = str_len(key), i = 0;
    out[0] = 0;
    while (wifi_buf[i]) {
        uint64_t s = i, n = 0;
        while (wifi_buf[i] && wifi_buf[i] != '\n') i++;
        if (i - s > klen + 1 && wifi_buf[s + klen] == ':') {
            uint64_t k = 0;
            while (k < klen && wifi_buf[s + k] == key[k]) k++;
            if (k == klen) {
                s += klen + 2;
                while (s < i && n + 1 < cap) out[n++] = wifi_buf[s++];
                out[n] = 0;
                return;
            }
        }
        if (wifi_buf[i]) i++;
    }
}

static uint64_t wifi_scan_gen(void) {
    const char *p = wifi_buf;
    uint64_t v = 0;
    while (*p && !str_prefix(p, "--- networks (scan ")) {
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    if (!*p) return 0;
    p += 19;
    while (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
    return v;
}

static void wifi_pad(const char *s, uint64_t width) {
    uint64_t n = str_len(s);
    icda_write(s);
    while (n++ < width) icda_write(" ");
}

/* Pretty-print the network rows of wifi_buf; returns how many. */
static int wifi_print_networks(const char *only_ssid, char *sec_out, int *saved_out) {
    const char *p = wifi_buf;
    int count = 0;
    while (*p && !str_prefix(p, "--- networks")) {
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
    if (!only_ssid) icda_write("  SIGNAL      DBM  CH   SECURITY  SAVED  NETWORK\n");
    while (*p) {
        char f[6][40];
        int fi = 0;
        uint64_t n = 0;
        for (int k = 0; k < 6; k++) f[k][0] = 0;
        while (*p && *p != '\n') {
            if (*p == '\t' && fi < 5) {
                f[fi][n] = 0;
                fi++;
                n = 0;
            } else if (n + 1 < sizeof(f[0])) {
                f[fi][n++] = *p;
            }
            p++;
        }
        f[fi][n] = 0;
        if (*p) p++;
        if (fi < 5) continue;
        count++;
        if (only_ssid) {
            if (str_eq(f[5], only_ssid)) {
                copy_text(sec_out, f[3], 16);
                *saved_out = str_eq(f[4], "saved");
                return 1;
            }
            continue;
        }
        {
            uint64_t pct = 0, bars;
            char meter[12];
            for (const char *q = f[0]; *q >= '0' && *q <= '9'; q++) pct = pct * 10 + (uint64_t)(*q - '0');
            bars = (pct + 19) / 20;
            for (uint64_t b = 0; b < 5; b++) meter[b] = b < bars ? '#' : '.';
            meter[5] = 0;
            icda_write("  ");
            wifi_pad(meter, 7);
            wifi_pad(f[0], 3);
            icda_write("% ");
            wifi_pad(f[1], 5);
            wifi_pad(f[2], 5);
            wifi_pad(f[3], 10);
            wifi_pad(str_eq(f[4], "saved") ? "yes" : "", 7);
            icda_write(f[5]);
            icda_write("\n");
        }
    }
    if (!only_ssid && count == 0) icda_write("  (no networks found yet)\n");
    return only_ssid ? 0 : count;
}

/* Read a password without echoing it. Returns its length, -1 if cancelled. */
static long wifi_read_password(char *out, uint64_t cap) {
    uint64_t n = 0;
    icda_write("password: ");
    for (;;) {
        long ch = shell_wait_key_byte(100);
        if (ch < 0) continue;
        if (ch == '\r' || ch == '\n') break;
        if (ch == 3 || ch == 27) {
            icda_write("\n");
            return -1;
        }
        if (ch == 8 || ch == 127) {
            if (n > 0) {
                n--;
                icda_backspace();
            }
            continue;
        }
        if (ch >= 32 && ch < 127 && n + 1 < cap) {
            out[n++] = (char)ch;
            icda_write("*");
        }
    }
    out[n] = 0;
    icda_write("\n");
    return (long)n;
}

/* Take one argument, honouring "double quotes". */
static char *wifi_take_arg(char **pp) {
    char *p = *pp, *start;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        start = ++p;
        while (*p && *p != '"') p++;
    } else {
        start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (*p) *p++ = 0;
    *pp = p;
    return start;
}

static void wifi_follow_connect(const char *ssid) {
    char state[32], last[32], err[120], ip[32];
    uint64_t start = icda_ticks();
    last[0] = 0;
    while (icda_ticks() - start < 6000) {   /* 60 s */
        wifi_cmd("status");
        wifi_read();
        wifi_field("state", state, sizeof(state));
        wifi_field("error", err, sizeof(err));
        if (!str_eq(state, last)) {
            icda_write("  ");
            icda_write(state);
            icda_write("\n");
            copy_text(last, state, sizeof(last));
        }
        if (str_eq(state, "connected")) {
            wifi_field("ip", ip, sizeof(ip));
            icda_write("connected to ");
            icda_write(ssid);
            icda_write(", address ");
            icda_write(ip[0] ? ip : "?");
            icda_write("\n");
            return;
        }
        if (str_eq(state, "no-ip")) {
            icda_write("associated, but DHCP failed (no IP address)\n");
            return;
        }
        if ((str_eq(state, "idle") || str_eq(state, "scanning") || str_eq(state, "error")) &&
            icda_ticks() - start > 100) {
            icda_write("not connected");
            if (err[0]) {
                icda_write(": ");
                icda_write(err);
            }
            icda_write("\n");
            return;
        }
        icda_sleep(25);
    }
    icda_write("still trying in the background; see 'wifi status' and 'wifi log'\n");
}

/* webkit: Surfer's engine (a Linux root in a "linux" folder).
 * "webkit install" copies it from another volume (the USB stick it came on)
 * to this system's partition, where Surfer finds it after a restart. */
static void shell_webkit(char *arg) {
    icda_disk_edit_t req;
    uint64_t finished, last_pct = 101;
    while (arg && *arg == ' ') arg++;
    if (!arg || !*arg || !str_eq(arg, "install")) {
        icda_write("usage: webkit install   (copy WebKit from a USB stick to this system)\n");
        return;
    }
    { char *z = (char *)&req; for (uint64_t i = 0; i < sizeof(req); i++) z[i] = 0; }
    req.op = 6;
    icda_disk_edit(&req);
    finished = req.partition;
    { char *z = (char *)&req; for (uint64_t i = 0; i < sizeof(req); i++) z[i] = 0; }
    req.op = 7;
    if (icda_disk_edit(&req) != 0) {
        icda_write("webkit: could not start (an install is already running?)\n");
        return;
    }
    icda_write("installing WebKit (about 530 MB, a few minutes)...\n");
    for (;;) {
        icda_sleep(50);
        { char *z = (char *)&req; for (uint64_t i = 0; i < sizeof(req); i++) z[i] = 0; }
        req.op = 6;
        icda_disk_edit(&req);
        if (req.sectors) {
            uint64_t pct = req.start_lba * 100 / req.sectors;
            if (pct != last_pct && pct % 5 == 0) {
                char line[48];
                int n = 0;
                uint64_t v = pct;
                char d[8];
                int k = 0;
                do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
                while (k) line[n++] = d[--k];
                line[n++] = '%';
                line[n++] = '\n';
                line[n] = 0;
                icda_write(line);
                last_pct = pct;
            }
        }
        if (req.partition != finished) break;
    }
    if ((int32_t)req.fs == 0) icda_write("WebKit installed. Restart ICDA, then Surfer uses it.\n");
    else if ((int32_t)req.fs == -3) icda_write("webkit: no WebKit found - plug in the ICDA USB stick (it has a linux folder)\n");
    else icda_write("webkit: the copy failed\n");
}

static void shell_wifi(char *arg) {
    char *p = arg ? arg : (char *)"";
    char *sub = wifi_take_arg(&p);

    if (str_eq(sub, "scan") || str_eq(sub, "connect")) {
        char state[32], err[120];
        wifi_cmd("status");
        wifi_read();
        wifi_field("state", state, sizeof(state));
        if (!state[0] || str_eq(state, "no-adapter") || str_eq(state, "starting") ||
            str_eq(state, "error") || str_eq(state, "radio-off")) {
            wifi_field("error", err, sizeof(err));
            icda_write("wifi: not ready (");
            icda_write(state[0] ? state : "no driver");
            icda_write(err[0] ? "): " : ")");
            icda_write(err);
            icda_write("\n");
            return;
        }
    }

    if (!*sub || str_eq(sub, "status")) {
        if (wifi_cmd("status") != 0) {
            icda_write("wifi: no /dev/wifi (kernel without Wi-Fi support)\n");
            return;
        }
        wifi_read();
        icda_write(wifi_buf);
        return;
    }
    if (str_eq(sub, "scan")) {
        uint64_t gen, start = icda_ticks();
        wifi_cmd("all");
        wifi_read();
        gen = wifi_scan_gen();
        if (wifi_cmd("scan") != 0) {
            icda_write("wifi: scan failed\n");
            return;
        }
        icda_write("scanning...\n");
        while (icda_ticks() - start < 1000) {
            icda_sleep(25);
            wifi_cmd("all");
            wifi_read();
            if (wifi_scan_gen() != gen) break;
        }
        wifi_print_networks(0, 0, 0);
        return;
    }
    if (str_eq(sub, "list")) {
        wifi_cmd("all");
        wifi_read();
        wifi_print_networks(0, 0, 0);
        return;
    }
    if (str_eq(sub, "connect")) {
        char *ssid = wifi_take_arg(&p);
        char *pass = wifi_take_arg(&p);
        char sec[16], cmd[128], typed[72];
        int saved = 0;
        if (!*ssid) {
            icda_write("usage: wifi connect <ssid> [password]   (quote SSIDs with spaces)\n");
            return;
        }
        sec[0] = 0;
        wifi_cmd("all");
        wifi_read();
        wifi_print_networks(ssid, sec, &saved);
        if (!*pass && !saved && sec[0] && !str_eq(sec, "open")) {
            long n = wifi_read_password(typed, sizeof(typed));
            if (n < 0) return;
            pass = typed;
        }
        cmd[0] = 0;
        append_text(cmd, "connect\t", sizeof(cmd));
        append_text(cmd, ssid, sizeof(cmd));
        append_text(cmd, "\t", sizeof(cmd));
        append_text(cmd, pass, sizeof(cmd));
        if (wifi_cmd(cmd) != 0) {
            icda_write("wifi: request rejected (SSID up to 32 bytes, password 8-63 characters)\n");
        } else {
            icda_write("connecting to ");
            icda_write(ssid);
            icda_write("...\n");
            wifi_follow_connect(ssid);
        }
        for (uint64_t i = 0; i < sizeof(cmd); i++) cmd[i] = 0;
        for (uint64_t i = 0; i < sizeof(typed); i++) typed[i] = 0;
        return;
    }
    if (str_eq(sub, "disconnect") || str_eq(sub, "restart")) {
        if (wifi_cmd(sub) != 0) icda_write("wifi: request failed\n");
        return;
    }
    if (str_eq(sub, "forget")) {
        char *ssid = wifi_take_arg(&p);
        char cmd[64];
        if (!*ssid) {
            icda_write("usage: wifi forget <ssid>\n");
            return;
        }
        cmd[0] = 0;
        append_text(cmd, "forget\t", sizeof(cmd));
        append_text(cmd, ssid, sizeof(cmd));
        if (wifi_cmd(cmd) != 0) icda_write("wifi: request failed\n");
        return;
    }
    if (str_eq(sub, "saved")) {
        wifi_cmd("saved");
        wifi_read();
        icda_write(wifi_buf[0] ? wifi_buf : "(no saved networks)\n");
        return;
    }
    if (str_eq(sub, "log")) {
        char *opt = wifi_take_arg(&p);
        wifi_cmd(str_eq(opt, "clear") ? "log clear" : "log");
        wifi_read();
        icda_write(wifi_buf);
        return;
    }
    if (str_eq(sub, "autoconnect")) {
        char *opt = wifi_take_arg(&p);
        wifi_cmd(str_eq(opt, "off") ? "autoconnect off" : "autoconnect on");
        return;
    }
    icda_write("usage: wifi [status|scan|list|connect <ssid> [password]|disconnect|\n"
               "            forget <ssid>|saved|log [clear]|autoconnect on|off|restart]\n");
}

static void shell_dispatch_one(char *line);

/* A line, with "> file" (replace) or ">> file" (add to the end) after it
 * taken off and the command's output put there; a ">" inside quotes is text. */
/* "a | b | c": each command's output is the next one's input (filters read
 * it: cat, grep, head, tail, wc, sort); "> file" or ">> file" at the end
 * takes the last output.  A "|" or ">" inside quotes is text. */
static char shell_pipe_buf[2][SHELL_CAPTURE_CAP];

static void shell_run_pipeline(char *line) {
    char quote = 0, *target = 0, *segs[8];
    int append = 0, nseg = 0;
    segs[nseg++] = line;
    for (uint64_t i = 0; line[i]; i++) {
        char c = line[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '|' && nseg < 8) {
            line[i] = 0;
            segs[nseg++] = &line[i + 1];
        } else if (c == '>') {
            append = line[i + 1] == '>';
            line[i] = 0;
            target = &line[i + 1 + append];
            break;
        }
    }
    shell_pipe_in = 0;
    shell_pipe_len = 0;
    for (int s = 0; s < nseg; s++) {
        int last = s == nseg - 1, capture = !last || target;
        if (capture) { shell_capture_len = 0; shell_capturing = 1; }
        shell_dispatch_one(segs[s]);
        shell_capturing = 0;
        if (!last) {
            char *buf = shell_pipe_buf[s & 1];
            for (uint64_t k = 0; k < shell_capture_len; k++) buf[k] = shell_capture_buf[k];
            buf[shell_capture_len] = 0;
            shell_pipe_in = buf;
            shell_pipe_len = shell_capture_len;
        }
    }
    shell_pipe_in = 0;
    if (!target) return;
    while (*target == ' ' || *target == '\t') target++;
    {
        uint64_t n = 0;
        while (target[n]) n++;
        while (n && (target[n - 1] == ' ' || target[n - 1] == '\t')) target[--n] = 0;
        if (n >= 2 && (target[0] == '"' || target[0] == '\'') && target[n - 1] == target[0]) {
            target[n - 1] = 0;
            target++;
        }
    }
    if (!*target) { icda_write("missing file name after >\n"); return; }
    {
        icda_stat_t st;
        long rc;
        if (append && (long)icda_stat(target, &st) >= 0)
            rc = (long)icda_write_file_at(target, st.size, shell_capture_buf, shell_capture_len);
        else
            rc = (long)icda_write_file(target, shell_capture_buf, shell_capture_len);
        if (rc < 0) {
            icda_write("could not write ");
            icda_write(target);
            icda_write("\n");
        }
    }
}

/* "a ; b" runs both, "a && b" runs b only if a worked; outside quotes */
static void shell_dispatch(char *line) {
    char quote = 0, *start = line;
    int need_ok = 0;
    for (uint64_t i = 0;; i++) {
        char c = line[i];
        int sep;
        if (c && quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        sep = !c || c == ';' ? 1 : (c == '&' && line[i + 1] == '&') ? 2 : 0;
        if (!sep) continue;
        line[i] = 0;
        if (!need_ok || !shell_failed) {
            shell_failed = 0;
            shell_run_pipeline(start);
        }
        if (!c) break;
        need_ok = sep == 2;
        i += (uint64_t)sep - 1;
        start = &line[i + 1];
    }
}

static void shell_dispatch_one(char *line) {
    char *arg = 0;
    uint64_t end = 0;
    while (*line == ' ' || *line == '\t') line++;
    /* "cat f.txt | sort" leaves "cat f.txt ": the space is not part of the name */
    while (line[end]) end++;
    while (end && (line[end - 1] == ' ' || line[end - 1] == '\t')) line[--end] = 0;
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
    if (str_eq(line, "grep")) { shell_grep(arg); return; }
    if (str_eq(line, "head")) { shell_head_tail(arg, 0); return; }
    if (str_eq(line, "tail")) { shell_head_tail(arg, 1); return; }
    if (str_eq(line, "wc")) { shell_wc(arg); return; }
    if (str_eq(line, "sort")) { shell_sort(arg); return; }
    if (str_eq(line, "echo")) { shell_echo(arg); return; }
    if (str_eq(line, "mkdir")) { shell_mkdir(arg); return; }
    if (str_eq(line, "rm")) { shell_rm(arg); return; }
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
    if (str_eq(line, "notify")) {             /* notify <title> [body]: posts a notification */
        char msg[256];
        uint64_t n = 0, i = 0;
        const char *pre = "Terminal|";
        if (!arg || !*arg) { icda_write("usage: notify <title> [body]\n"); return; }
        while (pre[n]) { msg[n] = pre[n]; n++; }
        while (arg[i] && arg[i] != ' ' && n + 1 < sizeof(msg)) msg[n++] = arg[i++];
        if (n + 1 < sizeof(msg)) msg[n++] = '\n';
        while (arg[i] == ' ') i++;
        while (arg[i] && n + 1 < sizeof(msg)) msg[n++] = arg[i++];
        (void)icda_write_file("/dev/notify", msg, n);
        return;
    }
    if (str_eq(line, "wifi")) { shell_wifi(arg); return; }
    if (str_eq(line, "webkit")) { shell_webkit(arg); return; }
    if (str_eq(line, "codex")) {
        char args[SHELL_LINE_CAP + 64];
        copy_text(args,"sh /bin/codex",sizeof(args));
        if(arg && *arg) {append_text(args," ",sizeof(args));append_text(args,arg,sizeof(args));}
        if(shell_spawn_and_wait("/bin/busybox",args)<0) icda_write("Could not start Codex CLI.\n");
        return;
    }
    if (str_eq(line, "run")) { shell_run_path(arg); return; }
    if (str_eq(line, "exit")) icda_exit(0);
    if (!shell_try_exec_command(line, arg)) {
        icda_write("unknown command: ");
        icda_write(line);
        icda_write("\n");
    }
}

/* A line from the keyboard; the password is not shown. */
static void sign_in_read(char *buf, int cap, int secret) {
    int n = 0;
    for (;;) {
        long c = icda_read_char();
        if (c == '\n' || c == '\r') break;
        if ((c == 8 || c == 127) && n > 0) {
            n--;
            if (!secret) icda_write("\b \b");
            continue;
        }
        if (c < 32 || c > 126 || n + 1 >= cap) continue;
        buf[n++] = (char)c;
        if (!secret) {
            char s[2] = { (char)c, 0 };
            icda_write(s);
        }
    }
    buf[n] = 0;
    icda_write("\n");
}

/* The text console (and the shell the kernel falls back to when the desktop
 * cannot start) signs a user in once accounts exist, as the desktop does:
 * otherwise Ctrl+Alt+F2 would be a way round every account's privacy.  A
 * shell started by a signed-in user (Terminal) is theirs already. */
static void shell_sign_in(void) {
    char who[96], list[64], name[32], pw[128];
    if (icda_account_whoami(who, sizeof(who)) > 0) return;
    if (icda_account_list(list, sizeof(list)) <= 0) return;      /* no accounts yet */
    icda_clear();
    for (;;) {
        icda_write("ICDA sign-in\nname: ");
        sign_in_read(name, sizeof(name), 0);
        icda_write("password: ");
        sign_in_read(pw, sizeof(pw), 1);
        if (icda_account_login(name, pw) == 0) break;
        icda_write("That name and password do not match.\n\n");
    }
    for (int i = 0; i < (int)sizeof(pw); i++) ((volatile char *)pw)[i] = 0;
}

uint64_t shell_main(uint64_t argc, char **argv) {
    char line[SHELL_LINE_CAP];
    if (argc > 1 && argv && argv[1] && argv[1][0]) {
        return shell_run_script(argv[1]) == 0 ? 0 : 1;
    }
    shell_sign_in();
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
