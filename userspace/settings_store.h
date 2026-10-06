#ifndef USERSPACE_SETTINGS_STORE_H
#define USERSPACE_SETTINGS_STORE_H





















#include "icda_sys.h"

#include <stdint.h>

#define ICDA_SETTINGS_PATH "/cfg/icda-settings"
#define ICDA_SETTINGS_FALLBACK "/etc/icda-settings"
#define ICDA_SETTINGS_CAP 256

#define ICDA_APPEARANCE_DARK  0
#define ICDA_APPEARANCE_LIGHT 1
#define ICDA_ACCENT_MAX       6

typedef struct {
    int vsync;
    int animations;
    int boot_anim;
    int audio;
    int appearance;
    int accent;
    int tz_minutes;
    int scale;
    int effects;            /* interface sounds (start menu, switches, ...) */
    int zoom;               /* display size in percent: 100, 125, 150, 175, 200 */
} icda_settings_t;

static __attribute__((unused)) void icda_settings_defaults(icda_settings_t *s) {
    if (!s) {
        return;
    }
    s->vsync = 1;
    s->animations = 1;
    s->boot_anim = 1;
    s->audio = 1;
    s->appearance = ICDA_APPEARANCE_DARK;
    s->accent = 0;
    s->tz_minutes = 0;
    s->scale = 0;
    s->effects = 1;
    s->zoom = 100;
}

static __attribute__((unused)) int icda_settings_key_is(const char *line, uint64_t key_len,
                                                        const char *key) {
    uint64_t i = 0;
    while (i < key_len && key[i] && line[i] == key[i]) {
        i++;
    }
    return i == key_len && key[i] == '\0';
}

static __attribute__((unused)) void icda_settings_apply_line(icda_settings_t *s, const char *line,
                                                             uint64_t len) {
    uint64_t i = 0;
    uint64_t key_len = 0;
    int value = -1;

    if (!s || !line || len == 0) {
        return;
    }
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (i >= len || line[i] == '#' || line[i] == '\n' || line[i] == '\r') {
        return;
    }
    while (i + key_len < len && line[i + key_len] != '=' && key_len < 16) {
        key_len++;
    }
    if (i + key_len >= len || line[i + key_len] != '=') {
        return;
    }
    if (key_len + 2 > len) {
        return;
    }
    if (icda_settings_key_is(line + i, key_len, "zoom")) {
        uint64_t j = i + key_len + 1;
        int v = 0;
        while (j < len && line[j] >= '0' && line[j] <= '9' && v < 1000) v = v * 10 + (line[j++] - '0');
        if (v == 100 || v == 125 || v == 150 || v == 175 || v == 200) s->zoom = v;
        return;
    }
    if (icda_settings_key_is(line + i, key_len, "tz")) {
        uint64_t j = i + key_len + 1;
        int sign = 1;
        int v = 0;
        if (j < len && (line[j] == '-' || line[j] == '+')) {
            if (line[j] == '-') sign = -1;
            j++;
        }
        while (j < len && line[j] >= '0' && line[j] <= '9' && v < 10000) {
            v = v * 10 + (line[j] - '0');
            j++;
        }
        if (v <= 14 * 60) s->tz_minutes = sign * v;
        return;
    }
    {
        char vc = line[i + key_len + 1];
        if (vc < '0' || vc > '9') {
            return;
        }
        value = vc - '0';
    }
    line += i;
    if (icda_settings_key_is(line, key_len, "scale")) {
        if (value <= 2) s->scale = value;
        return;
    }
    if (icda_settings_key_is(line, key_len, "accent")) {
        if (value <= ICDA_ACCENT_MAX) {
            s->accent = value;
        }
        return;
    }
    if (value > 1) {
        return;
    }
    if (icda_settings_key_is(line, key_len, "vsync")) {
        s->vsync = value;
    } else if (icda_settings_key_is(line, key_len, "animations")) {
        s->animations = value;
    } else if (icda_settings_key_is(line, key_len, "boot_anim")) {
        s->boot_anim = value;
    } else if (icda_settings_key_is(line, key_len, "audio")) {
        s->audio = value;
    } else if (icda_settings_key_is(line, key_len, "effects")) {
        s->effects = value;
    } else if (icda_settings_key_is(line, key_len, "appearance")) {
        s->appearance = value;
    }
}

static __attribute__((unused)) int icda_settings_load(icda_settings_t *s) {
    char buf[ICDA_SETTINGS_CAP];
    long n = 0;
    uint64_t count = 0;
    uint64_t pos = 0;
    uint64_t line_start = 0;

    if (!s) {
        return -1;
    }
    icda_settings_defaults(s);
    n = (long)icda_read_file(ICDA_SETTINGS_PATH, buf, sizeof(buf) - 1);
    if (n <= 0) {
        n = (long)icda_read_file(ICDA_SETTINGS_FALLBACK, buf, sizeof(buf) - 1);
        if (n <= 0) {
            return -1;
        }
    }
    count = (n > (long)(sizeof(buf) - 1)) ? (uint64_t)(sizeof(buf) - 1) : (uint64_t)n;
    while (pos <= count) {
        if (pos == count || buf[pos] == '\n') {
            icda_settings_apply_line(s, buf + line_start, pos - line_start);
            line_start = pos + 1;
        }
        pos++;
    }
    return 0;
}

static __attribute__((unused)) void icda_settings_put(char *dst, uint64_t cap, uint64_t *pos,
                                                      const char *text) {
    uint64_t i = 0;
    if (!dst || !pos || !text || *pos >= cap) {
        return;
    }
    while (text[i] && *pos + 1 < cap) {
        dst[*pos] = text[i];
        (*pos)++;
        i++;
    }
    dst[*pos < cap ? *pos : cap - 1] = '\0';
}

static __attribute__((unused)) int icda_settings_save(const icda_settings_t *s) {
    char buf[ICDA_SETTINGS_CAP];
    uint64_t pos = 0;
    long rc = 0;

    if (!s) {
        return -1;
    }
    icda_settings_put(buf, sizeof(buf), &pos, "# ICDA settings (0/1)\n");
    icda_settings_put(buf, sizeof(buf), &pos, "vsync=");
    icda_settings_put(buf, sizeof(buf), &pos, s->vsync ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "animations=");
    icda_settings_put(buf, sizeof(buf), &pos, s->animations ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "boot_anim=");
    icda_settings_put(buf, sizeof(buf), &pos, s->boot_anim ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "audio=");
    icda_settings_put(buf, sizeof(buf), &pos, s->audio ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "effects=");
    icda_settings_put(buf, sizeof(buf), &pos, s->effects ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "zoom=");
    icda_settings_put(buf, sizeof(buf), &pos,
                      s->zoom == 125 ? "125\n" : s->zoom == 150 ? "150\n" : s->zoom == 175 ? "175\n" :
                      s->zoom == 200 ? "200\n" : "100\n");
    icda_settings_put(buf, sizeof(buf), &pos, "appearance=");
    icda_settings_put(buf, sizeof(buf), &pos, s->appearance ? "1\n" : "0\n");
    {
        char accent_line[4];
        int a = s->accent < 0 ? 0 : (s->accent > ICDA_ACCENT_MAX ? 0 : s->accent);
        accent_line[0] = (char)('0' + a);
        accent_line[1] = '\n';
        accent_line[2] = '\0';
        icda_settings_put(buf, sizeof(buf), &pos, "accent=");
        icda_settings_put(buf, sizeof(buf), &pos, accent_line);
    }
    {
        char tz_line[12];
        int v = s->tz_minutes < 0 ? -s->tz_minutes : s->tz_minutes;
        int k = 0;
        char digits[6];
        int n = 0;
        if (s->tz_minutes < 0) tz_line[k++] = '-';
        do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 5);
        while (n > 0) tz_line[k++] = digits[--n];
        tz_line[k++] = '\n';
        tz_line[k] = '\0';
        icda_settings_put(buf, sizeof(buf), &pos, "tz=");
        icda_settings_put(buf, sizeof(buf), &pos, tz_line);
    }
    {
        char scale_line[3];
        scale_line[0] = (char)('0' + (s->scale >= 0 && s->scale <= 2 ? s->scale : 0));
        scale_line[1] = '\n';
        scale_line[2] = 0;
        icda_settings_put(buf, sizeof(buf), &pos, "scale=");
        icda_settings_put(buf, sizeof(buf), &pos, scale_line);
    }
    if (pos == 0 || pos >= sizeof(buf)) {
        return -1;
    }
    icda_mkdir("/cfg");
    rc = (long)icda_write_file(ICDA_SETTINGS_PATH, buf, pos);
    if (rc < 0) {
        return -1;
    }
    (void)icda_sync();
    return 0;
}

#endif 
