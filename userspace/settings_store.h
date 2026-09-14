#ifndef USERSPACE_SETTINGS_STORE_H
#define USERSPACE_SETTINGS_STORE_H

/* Slice C shared settings store (WM + Settings app + audio clients).
 *
 * Canonical path: /cfg/icda-settings (persistfs-backed, writable;
 * same best-effort persistence as /cfg/desktop.cfg - RAM-only on the
 * live ISO). Read falls back to /etc/icda-settings (seeded defaults).
 *
 * File format: one "key=value" per line, '#' comments and blank lines
 * ignored, unknown keys ignored. Values are 0/1; anything else leaves
 * the default in place:
 *
 *   vsync=1        tick-paced present (tear-free-ish) vs immediate
 *   animations=1   window open/close/min/max animations vs instant
 *   boot_anim=1    boot/power transition animation vs instant
 *   audio=1        audio enabled vs master mute (skip audio paths)
 *   theme=0        0 abyss dark, 1 frost light (frost-glass theme)
 *   accent=0       0 Blue, 1 Sky, 2 Teal, 3 Violet
 *   glass=1        0 opaque, 1 frost, 2 extra-clear
 *
 * All helpers are bounds-checked and freestanding-safe (no libc).
 */

#include "icda_sys.h"

#include <stdint.h>

#define ICDA_SETTINGS_PATH "/cfg/icda-settings"
#define ICDA_SETTINGS_FALLBACK "/etc/icda-settings"
#define ICDA_SETTINGS_CAP 384

typedef struct {
    int vsync;
    int animations;
    int boot_anim;
    int audio;
    int theme_mode;
    int accent;
    int glass;
} icda_settings_t;

static __attribute__((unused)) void icda_settings_defaults(icda_settings_t *s) {
    if (!s) {
        return;
    }
    s->vsync = 1;
    s->animations = 1;
    s->boot_anim = 1;
    s->audio = 1;
    s->theme_mode = 0;
    s->accent = 0;
    s->glass = 1;
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
    if (i + key_len + 2 > len) {
        return;
    }
    {
        char vc = line[i + key_len + 1];
        if (vc == '0' || vc == '1') {
            value = vc - '0';
        }
        /* wider ranges (accent 0-3, glass 0-2) are validated per key
         * below; 0/1-only keys require value != -1 via the branches. */
    }
    if (key_len == 5 && line[i] == 'v' && line[i + 1] == 's' && line[i + 2] == 'y' &&
        line[i + 3] == 'n' && line[i + 4] == 'c') {
        if (value != -1) {
            s->vsync = value;
        }
    } else if (key_len == 10 && line[i] == 'a' && line[i + 1] == 'n' && line[i + 2] == 'i' &&
               line[i + 3] == 'm' && line[i + 4] == 'a' && line[i + 5] == 't' &&
               line[i + 6] == 'i' && line[i + 7] == 'o' && line[i + 8] == 'n' &&
               line[i + 9] == 's') {
        if (value != -1) {
            s->animations = value;
        }
    } else if (key_len == 9 && line[i] == 'b' && line[i + 1] == 'o' && line[i + 2] == 'o' &&
               line[i + 3] == 't' && line[i + 4] == '_' && line[i + 5] == 'a' &&
               line[i + 6] == 'n' && line[i + 7] == 'i' && line[i + 8] == 'm') {
        if (value != -1) {
            s->boot_anim = value;
        }
    } else if (key_len == 5 && line[i] == 'a' && line[i + 1] == 'u' && line[i + 2] == 'd' &&
               line[i + 3] == 'i' && line[i + 4] == 'o') {
        if (value != -1) {
            s->audio = value;
        }
    } else if (key_len == 5 && line[i] == 't' && line[i + 1] == 'h' && line[i + 2] == 'e' &&
               line[i + 3] == 'm' && line[i + 4] == 'e') {
        char vc = line[i + key_len + 1];
        if (vc >= '0' && vc <= '1') {
            s->theme_mode = vc - '0';
        }
    } else if (key_len == 6 && line[i] == 'a' && line[i + 1] == 'c' && line[i + 2] == 'c' &&
               line[i + 3] == 'e' && line[i + 4] == 'n' && line[i + 5] == 't') {
        char vc = line[i + key_len + 1];
        if (vc >= '0' && vc <= '3') {
            s->accent = vc - '0';
        }
    } else if (key_len == 5 && line[i] == 'g' && line[i + 1] == 'l' && line[i + 2] == 'a' &&
               line[i + 3] == 's' && line[i + 4] == 's') {
        char vc = line[i + key_len + 1];
        if (vc >= '0' && vc <= '2') {
            s->glass = vc - '0';
        }
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
    int theme_mode;
    int accent;
    int glass;

    if (!s) {
        return -1;
    }
    theme_mode = s->theme_mode < 0 ? 0 : (s->theme_mode > 1 ? 1 : s->theme_mode);
    accent = s->accent < 0 ? 0 : (s->accent > 3 ? 3 : s->accent);
    glass = s->glass < 0 ? 0 : (s->glass > 2 ? 2 : s->glass);
    icda_settings_put(buf, sizeof(buf), &pos, "# ICDA settings (0/1 + theme/accent/glass)\n");
    icda_settings_put(buf, sizeof(buf), &pos, "vsync=");
    icda_settings_put(buf, sizeof(buf), &pos, s->vsync ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "animations=");
    icda_settings_put(buf, sizeof(buf), &pos, s->animations ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "boot_anim=");
    icda_settings_put(buf, sizeof(buf), &pos, s->boot_anim ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "audio=");
    icda_settings_put(buf, sizeof(buf), &pos, s->audio ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "theme=");
    icda_settings_put(buf, sizeof(buf), &pos, theme_mode ? "1\n" : "0\n");
    icda_settings_put(buf, sizeof(buf), &pos, "accent=");
    icda_settings_put(buf, sizeof(buf), &pos,
                      accent == 0 ? "0\n" : (accent == 1 ? "1\n" : (accent == 2 ? "2\n" : "3\n")));
    icda_settings_put(buf, sizeof(buf), &pos, "glass=");
    icda_settings_put(buf, sizeof(buf), &pos,
                      glass == 0 ? "0\n" : (glass == 1 ? "1\n" : "2\n"));
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

#endif /* USERSPACE_SETTINGS_STORE_H */
