#include "icda_sys.h"

#include <stdint.h>

#define SHELL_LINE_CAP 128
#define SHELL_BUF_CAP 1024
#define SHELL_JOB_CAP 16
#define SHELL_JOB_CMD_CAP 80
#define SHELL_HISTORY_CAP 16
#define SHELL_EDIT_BUF_CAP 4096
#define SHELL_EDIT_VIEW_ROWS 18
#define SHELL_EDIT_VIEW_COLS 68
#define SHELL_EDIT_REQUEST_PATH "/home/.edit.request"
#define SHELL_CURL_REQUEST_PATH "/home/.curl.request"

#define PROC_STATE_STOPPED 4
#define PROC_STATE_EXITED 5
#define PROC_STATE_REAPED 6

typedef struct {
    uint64_t pid;
    uint64_t active;
    uint64_t notified_done;
    char command[SHELL_JOB_CMD_CAP];
} shell_job_t;

static shell_job_t shell_job_table[SHELL_JOB_CAP];
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

static void write_uint_pad(uint64_t v, uint64_t width) {
    char buf[32];
    uint64_t i = sizeof(buf) - 1;
    uint64_t len;
    buf[i] = 0;
    if (v == 0) {
        buf[--i] = '0';
    } else {
        while (v && i > 0) {
            buf[--i] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    len = str_len(&buf[i]);
    while (len < width) {
        icda_write(" ");
        len++;
    }
    icda_write(&buf[i]);
}

static void write_repeat(char ch, uint64_t count) {
    char buf[65];
    uint64_t chunk = sizeof(buf) - 1;
    for (uint64_t i = 0; i < chunk; i++) buf[i] = ch;
    buf[chunk] = 0;
    while (count) {
        uint64_t take = count > chunk ? chunk : count;
        buf[take] = 0;
        icda_write(buf);
        buf[take] = ch;
        count -= take;
    }
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

    if (!path || !*path) {
        icda_write("usage: play <path>\n");
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

    if (!url[0] || !out_path[0]) {
        icda_write("usage: curl <http://host[:port]/path> <out-path>\n");
        return;
    }
    if (shell_resolve_path(out_path, resolved, sizeof(resolved)) != 0) {
        icda_write("curl: bad output path\n");
        return;
    }
    request[0] = 0;
    append_text(request, url, sizeof(request));
    append_text(request, "\n", sizeof(request));
    append_text(request, resolved, sizeof(request));
    shell_launch_foreground_request(SHELL_CURL_REQUEST_PATH, request, "/apps/curl.app", "usage: curl <http://host[:port]/path> <out-path>\n", 0);
}

static const char *proc_state_name(uint64_t state) {
    switch (state) {
        case 0: return "new";
        case 1: return "ready";
        case 2: return "running";
        case 3: return "blocked";
        case 4: return "stopped";
        case 5: return "exited";
        case 6: return "reaped";
        default: return "unknown";
    }
}

static shell_job_t *shell_find_job(uint64_t pid) {
    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        if (shell_job_table[i].active && shell_job_table[i].pid == pid) {
            return &shell_job_table[i];
        }
    }
    return 0;
}

static shell_job_t *shell_add_job(uint64_t pid, const char *command) {
    shell_job_t *job = shell_find_job(pid);
    if (job) {
        job->notified_done = 0;
        copy_text(job->command, command, sizeof(job->command));
        return job;
    }
    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        if (!shell_job_table[i].active) {
            shell_job_table[i].active = 1;
            shell_job_table[i].pid = pid;
            shell_job_table[i].notified_done = 0;
            copy_text(shell_job_table[i].command, command, sizeof(shell_job_table[i].command));
            return &shell_job_table[i];
        }
    }
    return 0;
}

static void shell_remove_job(uint64_t pid) {
    shell_job_t *job = shell_find_job(pid);
    if (!job) return;
    job->active = 0;
    job->pid = 0;
    job->notified_done = 0;
    job->command[0] = 0;
}

static void shell_poll_jobs(int notify) {
    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        icda_proc_info_t info;
        shell_job_t *job = &shell_job_table[i];

        if (!job->active) continue;
        if ((long)icda_proc_info(job->pid, &info) < 0) {
            job->active = 0;
            job->pid = 0;
            job->command[0] = 0;
            continue;
        }
        if (info.state == PROC_STATE_REAPED) {
            job->active = 0;
            job->pid = 0;
            job->command[0] = 0;
            continue;
        }
        if (notify && !job->notified_done && info.state == PROC_STATE_EXITED) {
            icda_write("[job ");
            write_uint(info.pid);
            icda_write("] done exit=");
            write_uint(info.exit_code);
            icda_write(" ");
            icda_write(job->command);
            icda_write("\n");
            job->notified_done = 1;
        }
    }
}

static void shell_history_add(const char *line) {
    if (!line || !*line) return;
    if (shell_history_count && str_eq(shell_history[(shell_history_count - 1) % SHELL_HISTORY_CAP], line)) {
        return;
    }
    copy_text(shell_history[shell_history_count % SHELL_HISTORY_CAP], line, SHELL_LINE_CAP);
    shell_history_count++;
}

static void shell_rewrite_line(const char *line, uint64_t *shown_len) {
    while (*shown_len) {
        icda_backspace();
        (*shown_len)--;
    }
    icda_write(line);
    *shown_len = str_len(line);
}

static void shell_render_line(const char *line, uint64_t len, uint64_t cursor, uint64_t prompt_x, uint64_t prompt_y, uint64_t *shown_len) {
    uint64_t i;
    uint64_t old_len = shown_len ? *shown_len : 0;
    uint64_t pad = old_len > len ? old_len - len : 0;

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
    (void)visible;
}

static void shell_cursor_hide(int *visible) {
    (void)visible;
}

static long shell_wait_key_byte(uint64_t timeout_ticks) {
    return icda_read_char_timeout(timeout_ticks);
}

static long shell_read_key(void) {
    long c = shell_wait_key_byte(0);
    if (c != 27) {
        return c;
    }

    {
        long c1 = shell_wait_key_byte(2);
        if (c1 != '[') {
            return c;
        }

        switch (shell_wait_key_byte(2)) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case '3':
                if (shell_wait_key_byte(2) == '~') return KEY_DELETE;
                return c;
            default: return c;
        }
    }
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
    (void)icda_console_cursor(&prompt_x, &prompt_y);
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

        if (c == '\b') {
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
    for (uint64_t i = 0; i < len; i++) {
        if (buf[i] == '\n') lines++;
    }
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
    if (row < 3) return 0;
    if (row + 4 < SHELL_EDIT_VIEW_ROWS) return 0;
    return row - 3;
}

static void editor_write_line_segment(const char *buf, uint64_t len, uint64_t start, uint64_t cursor, uint64_t *cursor_shown) {
    uint64_t end = editor_line_end(buf, len, start);
    uint64_t pos = start;
    uint64_t shown = 0;
    while (pos < end) {
        if (shown >= SHELL_EDIT_VIEW_COLS) break;
        if (pos == cursor && !*cursor_shown && shown < SHELL_EDIT_VIEW_COLS) {
            icda_write("|");
            *cursor_shown = 1;
            shown++;
            if (shown >= SHELL_EDIT_VIEW_COLS) break;
        }
        {
            char out[2] = { buf[pos], 0 };
            icda_write(out);
            shown++;
        }
        pos++;
    }
    if (cursor == end && !*cursor_shown && shown < SHELL_EDIT_VIEW_COLS) {
        icda_write("|");
        *cursor_shown = 1;
        shown++;
    }
    if (shown < SHELL_EDIT_VIEW_COLS) {
        write_repeat(' ', SHELL_EDIT_VIEW_COLS - shown);
    }
}

static void editor_redraw(const char *path, const char *buf, uint64_t len, uint64_t cursor, int modified) {
    uint64_t top_row = editor_view_top_for_cursor(buf, cursor);
    uint64_t start = editor_find_row_start(buf, len, top_row);
    uint64_t line_no = top_row + 1;
    uint64_t cursor_shown = 0;
    uint64_t lines_total = editor_line_count(buf, len);

    icda_set_cursor(0, 0);
    icda_write("####################################################################\n");
    icda_write("# ICDA editor  ");
    icda_write(path);
    icda_write(modified ? "   *modified" : "   saved");
    write_repeat(' ', 68);
    icda_write("\n");
    icda_write("# Ctrl+S save   Ctrl+X exit   arrows move   backspace/delete       #\n");
    icda_write("####################################################################\n");

    for (uint64_t row = 0; row < SHELL_EDIT_VIEW_ROWS; row++) {
        if (start > len) start = len;
        write_uint_pad(line_no, 4);
        icda_write(" # ");
        if (start <= len) {
            editor_write_line_segment(buf, len, start, cursor, &cursor_shown);
            start = editor_line_end(buf, len, start);
            if (start < len && buf[start] == '\n') start++;
        } else {
            write_repeat(' ', SHELL_EDIT_VIEW_COLS);
        }
        icda_write(" #\n");
        line_no++;
    }

    icda_write("####################################################################\n");
    icda_write("# Ln ");
    write_uint(editor_line_number(buf, cursor));
    icda_write("/");
    write_uint(lines_total);
    icda_write("   Col ");
    write_uint(editor_column(buf, len, cursor) + 1);
    icda_write("   Size ");
    write_uint(len);
    icda_write(" bytes");
    if (!cursor_shown) {
        icda_write("   [cursor off-screen]");
    }
    write_repeat(' ', 68);
    icda_write("\n");
    icda_write("####################################################################\n");
}

static void editor_insert_char(char *buf, uint64_t *len, uint64_t *cursor, char ch, uint64_t cap) {
    if (!buf || !len || !cursor || *len + 1 >= cap) return;
    for (uint64_t i = *len; i > *cursor; i--) {
        buf[i] = buf[i - 1];
    }
    buf[*cursor] = ch;
    (*len)++;
    (*cursor)++;
    buf[*len] = 0;
}

static void editor_backspace(char *buf, uint64_t *len, uint64_t *cursor) {
    if (!buf || !len || !cursor || *cursor == 0) return;
    for (uint64_t i = *cursor - 1; i < *len; i++) {
        buf[i] = buf[i + 1];
    }
    (*cursor)--;
    (*len)--;
    buf[*len] = 0;
}

static void editor_delete(char *buf, uint64_t *len, uint64_t *cursor) {
    if (!buf || !len || !cursor || *cursor >= *len) return;
    for (uint64_t i = *cursor; i < *len; i++) {
        buf[i] = buf[i + 1];
    }
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
    icda_write("icda:");
    icda_write(cwd);
    icda_write(" ");
}

static void shell_help(void) {
    icda_write("commands: help clear pwd cd ls cat mkdir touch write stat install sync storage mount play stop edit diskman curl run exit\n");
}

static void shell_money(void) {
    icda_write("            *            \n");
    icda_write("           * *           \n");
    icda_write("          *   *          \n");
    icda_write("         *     *         \n");
    icda_write("*********       *********\n");
    icda_write(" *                     * \n");
    icda_write("  *                   *  \n");
    icda_write("   *                 *   \n");
    icda_write("    ***************    \n");
    icda_write("   *                 *   \n");
    icda_write("  *                   *  \n");
    icda_write(" *                     * \n");
    icda_write("*********       *********\n");
    icda_write("         *     *         \n");
    icda_write("          *   *          \n");
    icda_write("           * *           \n");
    icda_write("            *            \n");
    shell_play_wav_path("/usr/share/audio/hava_clip.wav");
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
    if (!shell_add_job(pid, path)) {
        icda_write("warning: job table full, process still running\n");
    }
    icda_write("spawned pid=");
    write_uint(pid);
    icda_write("\n");
}

static void shell_wait_pid(const char *arg) {
    uint64_t pid = 0;
    uint64_t code;

    if (!arg || !*arg) {
        icda_write("usage: wait <pid>\n");
        return;
    }
    if (!parse_uint64(arg, &pid)) {
        icda_write("usage: wait <pid>\n");
        return;
    }
    code = icda_waitpid(pid);
    if ((long)code < 0) {
        icda_write("wait failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    shell_remove_job(pid);
    icda_write("pid=");
    write_uint(pid);
    icda_write(" exit=");
    write_uint(code);
    icda_write("\n");
}

static void shell_yield_once(void) {
    icda_yield();
}

static void shell_sync(void) {
    if ((long)icda_sync() < 0) {
        icda_write("sync failed\n");
        return;
    }
    icda_write("synced\n");
}

static void shell_install(void) {
    uint64_t files = 0;
    uint64_t bytes = 0;

    if ((long)icda_install_system(&files, &bytes) < 0) {
        icda_write("install failed\n");
        return;
    }

    icda_write("installed ");
    write_uint(files);
    icda_write(" files, ");
    write_uint(bytes);
    icda_write(" bytes persisted\n");
    icda_write("details: /system/install/state.txt and /system/install/manifest.txt\n");
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

static void shell_sleep_ticks(const char *arg) {
    uint64_t ticks = 0;

    if (!arg || !*arg || !parse_uint64(arg, &ticks)) {
        icda_write("usage: sleep <ticks>\n");
        return;
    }
    icda_sleep(ticks);
}

static void shell_jobs(void) {
    int any = 0;

    shell_poll_jobs(0);
    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        icda_proc_info_t info;
        shell_job_t *job = &shell_job_table[i];

        if (!job->active) continue;
        if ((long)icda_proc_info(job->pid, &info) < 0) continue;

        any = 1;
        icda_write("pid=");
        write_uint(info.pid);
        icda_write(" sid=");
        write_uint(info.sid);
        icda_write(" pgid=");
        write_uint(info.pgid);
        icda_write(" state=");
        icda_write(proc_state_name(info.state));
        icda_write(" exit=");
        write_uint(info.exit_code);
        icda_write(" cmd=");
        icda_write(job->command);
        icda_write("\n");
    }

    if (!any) {
        icda_write("no background jobs\n");
    }
}

static void shell_wait_all(void) {
    int waited = 0;

    for (uint64_t i = 0; i < SHELL_JOB_CAP; i++) {
        uint64_t code;
        shell_job_t *job = &shell_job_table[i];

        if (!job->active) continue;
        code = icda_waitpid(job->pid);
        if ((long)code >= 0) {
            icda_write("pid=");
            write_uint(job->pid);
            icda_write(" exit=");
            write_uint(code);
            icda_write("\n");
        }
        shell_remove_job(job->pid);
        waited = 1;
    }

    if (!waited) {
        icda_write("no background jobs\n");
    }
}

static void shell_fg(const char *arg) {
    uint64_t pid = 0;
    icda_proc_info_t info;

    if (!arg || !*arg || !parse_uint64(arg, &pid)) {
        icda_write("usage: fg <pid>\n");
        return;
    }
    if ((long)icda_proc_info(pid, &info) >= 0 && info.state == PROC_STATE_STOPPED) {
        if ((long)icda_resume(pid) < 0) {
            icda_write("resume failed: ");
            write_uint(pid);
            icda_write("\n");
            return;
        }
    }
    shell_wait_pid(arg);
}

static void shell_stop_pid(const char *arg) {
    uint64_t pid = 0;

    if (!arg || !*arg || !parse_uint64(arg, &pid)) {
        icda_write("usage: stop <pid>\n");
        return;
    }
    if ((long)icda_suspend(pid) < 0) {
        icda_write("stop failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    icda_write("stopped pid=");
    write_uint(pid);
    icda_write("\n");
}

static void shell_resume_pid(const char *arg) {
    uint64_t pid = 0;

    if (!arg || !*arg || !parse_uint64(arg, &pid)) {
        icda_write("usage: resume <pid>\n");
        return;
    }
    if ((long)icda_resume(pid) < 0) {
        icda_write("resume failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    icda_write("resumed pid=");
    write_uint(pid);
    icda_write("\n");
}

static void shell_kill_pid(const char *arg) {
    uint64_t pid = 0;

    if (!arg || !*arg || !parse_uint64(arg, &pid)) {
        icda_write("usage: kill <pid>\n");
        return;
    }
    if ((long)icda_kill(pid, 143) < 0) {
        icda_write("kill failed: ");
        write_uint(pid);
        icda_write("\n");
        return;
    }
    icda_write("killed pid=");
    write_uint(pid);
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
    if (str_eq(line, "pwd")) { shell_pwd(); return; }
    if (str_eq(line, "cd")) { if ((long)icda_chdir((arg && *arg) ? arg : "/") < 0) icda_write("cd failed\n"); return; }
    if (str_eq(line, "ls")) { shell_ls(arg); return; }
    if (str_eq(line, "cat")) { shell_cat(arg); return; }
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
    if (!shell_try_exec_command(line)) {
        icda_write("unknown command: ");
        icda_write(line);
        icda_write("\n");
    }
}

uint64_t shell_main(void) {
    char line[SHELL_LINE_CAP];
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
