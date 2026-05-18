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
#define SHELL_AUDIO_BUF_CAP 320000
#define SHELL_AUDIO_PLAY_CHUNK 65535
#ifndef SHELL_AUTOTEST
#define SHELL_AUTOTEST 0
#endif

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
static uint8_t shell_audio_buf[SHELL_AUDIO_BUF_CAP];
static uint8_t shell_audio_out_buf[SHELL_AUDIO_BUF_CAP];

enum {
    KEY_SPECIAL_BASE = 256,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_DELETE
};

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

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
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

static int shell_parse_wav(const uint8_t *buf, uint64_t size,
                           uint16_t *channels_out, uint32_t *rate_out,
                           uint16_t *bits_out, const uint8_t **data_out,
                           uint32_t *data_size_out) {
    uint64_t off = 12;
    uint16_t fmt_tag = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0;
    const uint8_t *data = 0;
    uint32_t data_size = 0;
    int have_fmt = 0;

    if (!buf || size < 44) return -1;
    if (!(buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F')) return -1;
    if (!(buf[8] == 'W' && buf[9] == 'A' && buf[10] == 'V' && buf[11] == 'E')) return -1;

    while (off + 8 <= size) {
        const uint8_t *chunk = &buf[off];
        uint32_t chunk_size = read_le32(chunk + 4);
        uint64_t next = off + 8ULL + chunk_size + (chunk_size & 1U);
        if (next > size) break;

        if (chunk[0] == 'f' && chunk[1] == 'm' && chunk[2] == 't' && chunk[3] == ' ') {
            if (chunk_size < 16) return -1;
            fmt_tag = read_le16(chunk + 8);
            channels = read_le16(chunk + 10);
            rate = read_le32(chunk + 12);
            bits = read_le16(chunk + 22);
            have_fmt = 1;
        } else if (chunk[0] == 'd' && chunk[1] == 'a' && chunk[2] == 't' && chunk[3] == 'a') {
            data = chunk + 8;
            data_size = chunk_size;
        }
        off = next;
    }

    if (!have_fmt || !data || fmt_tag != 1 || channels == 0 || rate == 0) return -1;
    if (!(bits == 8 || bits == 16)) return -1;

    *channels_out = channels;
    *rate_out = rate;
    *bits_out = bits;
    *data_out = data;
    *data_size_out = data_size;
    return 0;
}

static int32_t shell_sample_at(const uint8_t *data, uint32_t frame_index, uint16_t channels, uint16_t bits) {
    uint32_t sample_index = frame_index * channels;
    if (bits == 8) {
        int32_t sum = 0;
        for (uint16_t ch = 0; ch < channels; ch++) {
            sum += ((int32_t)data[sample_index + ch] - 128) << 8;
        }
        return sum / channels;
    } else {
        int32_t sum = 0;
        const uint8_t *p = data + (sample_index * 2U);
        for (uint16_t ch = 0; ch < channels; ch++) {
            int16_t s = (int16_t)read_le16(p + (ch * 2U));
            sum += s;
        }
        return sum / channels;
    }
}

static uint32_t shell_convert_wav_to_u8_mono(const uint8_t *data, uint32_t frames,
                                             uint16_t channels, uint16_t bits,
                                             uint32_t input_rate, uint32_t output_rate,
                                             uint8_t *out, uint32_t out_cap) {
    uint64_t out_frames = ((uint64_t)frames * (uint64_t)output_rate + (uint64_t)input_rate - 1ULL) / (uint64_t)input_rate;

    if (out_frames > out_cap) {
        out_frames = out_cap;
    }

    for (uint32_t i = 0; i < (uint32_t)out_frames; i++) {
        uint32_t source_index = (uint32_t)(((uint64_t)i * (uint64_t)input_rate) / (uint64_t)output_rate);
        if (source_index >= frames) {
            source_index = frames - 1;
        }
        int32_t sample = shell_sample_at(data, source_index, channels, bits);
        sample += 32768;
        if (sample < 0) sample = 0;
        if (sample > 65535) sample = 65535;
        out[i] = (uint8_t)(sample >> 8);
    }

    return (uint32_t)out_frames;
}

static void shell_play_wav_path(const char *path) {
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t sample_rate = 0;
    const uint8_t *pcm = 0;
    uint32_t pcm_size = 0;
    uint32_t total_frames;
    uint32_t pcm_u8_size;
    uint32_t output_rate;
    long size;

    if (!path || !*path) {
        icda_write("usage: play <path>\n");
        return;
    }

    size = (long)icda_read_file(path, (char *)shell_audio_buf, sizeof(shell_audio_buf));
    if (size < 0) {
        icda_write("play failed: ");
        icda_write(path);
        icda_write("\n");
        return;
    }

    if (shell_parse_wav(shell_audio_buf, (uint64_t)size, &channels, &sample_rate, &bits, &pcm, &pcm_size) != 0) {
        icda_write("unsupported wav format\n");
        return;
    }

    total_frames = pcm_size / (channels * (bits / 8U));
    output_rate = sample_rate;
    if (total_frames > SHELL_AUDIO_PLAY_CHUNK) {
        output_rate = (uint32_t)(((uint64_t)sample_rate * (uint64_t)SHELL_AUDIO_PLAY_CHUNK) / (uint64_t)total_frames);
        if (output_rate < 2000U) {
            output_rate = 2000U;
        }
    }

    pcm_u8_size = shell_convert_wav_to_u8_mono(pcm, total_frames, channels, bits,
                                               sample_rate, output_rate,
                                               shell_audio_out_buf, sizeof(shell_audio_out_buf));
    if (pcm_u8_size == 0) {
        icda_write("audio conversion failed\n");
        return;
    }
    if ((long)icda_play_pcm_u8(shell_audio_out_buf, pcm_u8_size, output_rate) < 0) {
        icda_write("pcm playback unavailable\n");
    }
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

static long shell_read_key(void) {
    long c = (long)icda_read_char();
    if (c != 27) {
        return c;
    }

    {
        long c1 = (long)icda_read_char();
        if (c1 != '[') {
            return c;
        }

        switch ((long)icda_read_char()) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case '3':
                if ((long)icda_read_char() == '~') return KEY_DELETE;
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
            "help","clear","pid","ps","jobs","pwd","cd","ls","cat","mkdir","touch","write","stat","sync","storage","money","play",
            "run","spawn","bg","fg","wait","waitall","stop","resume","kill","sleep","yield","edit","exit"
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
    uint64_t shown_len = 0;
    uint64_t history_cursor = shell_history_count;

    if (!line || cap == 0) return -1;
    line[0] = 0;

    for (;;) {
        long c = shell_read_key();
        if (c < 0) continue;

        if (c == '\r' || c == '\n') {
            icda_write("\n");
            line[len] = 0;
            if (len) shell_history_add(line);
            return (int)len;
        }

        if (c == '\t') {
            if (shell_autocomplete(line, cap)) {
                len = str_len(line);
                shell_rewrite_line(line, &shown_len);
            }
            continue;
        }

        if (c == '\b') {
            if (len) {
                len--;
                line[len] = 0;
                shell_rewrite_line(line, &shown_len);
            }
            continue;
        }

        if (c == KEY_UP) {
            if (shell_history_count) {
                if (history_cursor > 0) history_cursor--;
                copy_text(line, shell_history[history_cursor % SHELL_HISTORY_CAP], cap);
                len = str_len(line);
                shell_rewrite_line(line, &shown_len);
            }
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
            shell_rewrite_line(line, &shown_len);
            continue;
        }

        if (c >= 32 && c <= 126 && len + 1 < cap) {
            line[len++] = (char)c;
            line[len] = 0;
            {
                char out[2] = {(char)c, 0};
                icda_write(out);
                shown_len++;
            }
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
    char buf[SHELL_EDIT_BUF_CAP];
    long ret;
    uint64_t len = 0;
    uint64_t cursor = 0;
    int modified = 0;

    if (!path || !*path) {
        icda_write("usage: edit <path>\n");
        return;
    }

    ret = (long)icda_read_file(path, buf, sizeof(buf) - 1);
    if (ret < 0) {
        buf[0] = 0;
        len = 0;
    } else {
        len = (uint64_t)ret;
        buf[len] = 0;
    }

    icda_clear();
    editor_redraw(path, buf, len, cursor, modified);
    for (;;) {
        long c = shell_read_key();
        if (c < 0) continue;

        if (c == 24) {
            icda_clear();
            return;
        }
        if (c == 19) {
            if ((long)icda_write_file(path, buf, len) < 0) {
                icda_write("\nsave failed");
            } else {
                modified = 0;
            }
            editor_redraw(path, buf, len, cursor, modified);
            continue;
        }
        if (c == '\b') {
            editor_backspace(buf, &len, &cursor);
            modified = 1;
            editor_redraw(path, buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_DELETE) {
            editor_delete(buf, &len, &cursor);
            modified = 1;
            editor_redraw(path, buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_LEFT) {
            editor_move_left(&cursor);
            editor_redraw(path, buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_RIGHT) {
            editor_move_right(len, &cursor);
            editor_redraw(path, buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_UP) {
            editor_move_vertical(buf, len, &cursor, -1);
            editor_redraw(path, buf, len, cursor, modified);
            continue;
        }
        if (c == KEY_DOWN) {
            editor_move_vertical(buf, len, &cursor, 1);
            editor_redraw(path, buf, len, cursor, modified);
            continue;
        }
        if ((c == '\r' || c == '\n')) {
            editor_insert_char(buf, &len, &cursor, '\n', sizeof(buf));
            modified = 1;
            editor_redraw(path, buf, len, cursor, modified);
            continue;
        }
        if (c >= 32 && c <= 126) {
            editor_insert_char(buf, &len, &cursor, (char)c, sizeof(buf));
            modified = 1;
            editor_redraw(path, buf, len, cursor, modified);
        }
    }
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
    icda_write("user commands: help clear pid ps jobs pwd cd ls cat mkdir touch write stat sync storage money play edit run spawn bg fg wait waitall stop resume kill sleep yield exit\n");
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
    shell_play_wav_path("/usr/share/audio/hava.wav");
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

static void shell_storage(void) {
    char buf[SHELL_BUF_CAP];
    long ret = (long)icda_storage_info(buf, sizeof(buf));
    if (ret < 0) {
        icda_write("storage query failed\n");
        return;
    }
    icda_write(buf);
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

#if SHELL_AUTOTEST
static void shell_autotest(void) {
    uint64_t pid1;
    uint64_t pid2;
    uint64_t pid3;
    uint64_t code;

    icda_write("[selftest] spawn ticker x2\n");
    pid1 = icda_spawn("/apps/ticker.app");
    pid2 = icda_spawn("/apps/ticker.app");
    if ((long)pid1 < 0 || (long)pid2 < 0) {
        icda_write("[selftest] spawn failed\n");
        return;
    }
    icda_write("[selftest] pid1=");
    write_uint(pid1);
    icda_write(" pid2=");
    write_uint(pid2);
    icda_write("\n");
    shell_add_job(pid1, "/apps/ticker.app");
    shell_add_job(pid2, "/apps/ticker.app");
    icda_sleep(3);
    if ((long)icda_suspend(pid2) < 0) {
        icda_write("[selftest] suspend failed\n");
        return;
    }
    icda_write("[selftest] stopped pid2\n");
    icda_sleep(6);
    if ((long)icda_resume(pid2) < 0) {
        icda_write("[selftest] resume failed\n");
        return;
    }
    icda_write("[selftest] resumed pid2\n");
    pid3 = icda_spawn("/apps/ticker.app");
    if ((long)pid3 < 0) {
        icda_write("[selftest] spawn pid3 failed\n");
        return;
    }
    shell_add_job(pid3, "/apps/ticker.app");
    icda_sleep(1);
    if ((long)icda_kill(pid3, 143) < 0) {
        icda_write("[selftest] kill failed\n");
        return;
    }
    code = icda_waitpid(pid3);
    if ((long)code < 0) {
        icda_write("[selftest] wait pid3 failed\n");
        return;
    }
    icda_write("[selftest] pid3 exit=");
    write_uint(code);
    icda_write("\n");
    shell_remove_job(pid3);
    icda_sleep(10);
    shell_poll_jobs(1);
    shell_wait_all();
    icda_write("[selftest] done\n");
}
#endif

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
    if (str_eq(line, "jobs")) { shell_jobs(); return; }
    if (str_eq(line, "pwd")) { shell_pwd(); return; }
    if (str_eq(line, "cd")) { if ((long)icda_chdir((arg && *arg) ? arg : "/") < 0) icda_write("cd failed\n"); return; }
    if (str_eq(line, "ls")) { shell_ls(arg); return; }
    if (str_eq(line, "cat")) { shell_cat(arg); return; }
    if (str_eq(line, "mkdir")) { shell_mkdir(arg); return; }
    if (str_eq(line, "touch")) { shell_touch(arg); return; }
    if (str_eq(line, "write")) { shell_write_file(arg); return; }
    if (str_eq(line, "stat")) { shell_stat(arg); return; }
    if (str_eq(line, "sync")) { shell_sync(); return; }
    if (str_eq(line, "storage")) { shell_storage(); return; }
    if (str_eq(line, "money")) { shell_money(); return; }
    if (str_eq(line, "play")) { shell_play_wav_path(arg); return; }
    if (str_eq(line, "edit")) { shell_edit(arg); return; }
    if (str_eq(line, "run")) { shell_run_path(arg); return; }
    if (str_eq(line, "spawn")) { shell_spawn_path(arg); return; }
    if (str_eq(line, "bg")) { shell_spawn_path(arg); return; }
    if (str_eq(line, "fg")) { shell_fg(arg); return; }
    if (str_eq(line, "wait")) { shell_wait_pid(arg); return; }
    if (str_eq(line, "waitall")) { shell_wait_all(); return; }
    if (str_eq(line, "stop")) { shell_stop_pid(arg); return; }
    if (str_eq(line, "resume")) { shell_resume_pid(arg); return; }
    if (str_eq(line, "kill")) { shell_kill_pid(arg); return; }
    if (str_eq(line, "sleep")) { shell_sleep_ticks(arg); return; }
    if (str_eq(line, "yield")) { shell_yield_once(); return; }
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
    icda_write("icda user shell\n\n");
#if SHELL_AUTOTEST
    shell_autotest();
#endif
    for (;;) {
        shell_poll_jobs(1);
        print_prompt();
        if (shell_read_line(line, sizeof(line)) < 0) {
            continue;
        }
        shell_dispatch(line);
    }
}
