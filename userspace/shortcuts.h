#ifndef USERSPACE_SHORTCUTS_H
#define USERSPACE_SHORTCUTS_H

/*
 * Global keyboard shortcuts, shared by the window manager (which acts on
 * them) and Settings > Keyboard (which edits them).
 *
 * The keyboard driver reports system-level combinations as a chord: the
 * byte 0x82, then 0x40 | modifiers, then a key code.  These are the Windows
 * key with anything (or tapped alone), Ctrl+Alt+key, Alt+F-key, plain
 * F1..F10, Alt+Tab and Ctrl+Shift+Esc; everything else stays a normal key for
 * the focused app.  Bindings live in /cfg/shortcuts as "action=combo" lines
 * ("terminal=ctrl+alt+t"); missing actions use the defaults below.
 */

#include "icda_sys.h"
#include <stdint.h>

#define SC_CHORD        0x82
#define SC_MOD_SHIFT    1
#define SC_MOD_CTRL     2
#define SC_MOD_ALT      4
#define SC_MOD_SUPER    8

/* key codes in a chord besides lower-case letters, digits and punctuation */
#define SC_KEY_NONE     0x00          /* modifier tapped alone (Windows key) */
#define SC_KEY_TAB      0x09
#define SC_KEY_ENTER    0x0D
#define SC_KEY_ESC      0x1B
#define SC_KEY_SPACE    0x20
#define SC_KEY_UP       0xE1
#define SC_KEY_DOWN     0xE2
#define SC_KEY_LEFT     0xE3
#define SC_KEY_RIGHT    0xE4
#define SC_KEY_DELETE   0xE5
#define SC_KEY_F1       0xF1          /* .. SC_KEY_F1 + 11 = F12 */

/* a chord forwarded to an app (Settings capturing a new shortcut) */
#define SC_APP_KEY_BASE 0x1000000u
#define SC_CAPTURE_FLAG "/cfg/shortcut-capture"
#define SC_PATH         "/cfg/shortcuts"

typedef struct {
    const char *id;
    const char *label;
    const char *def;
} sc_action_t;

static const sc_action_t sc_actions[] = {
    { "start",    "Open the start menu",  "super" },
    { "explorer", "Open Explorer",        "super+e" },
    { "terminal", "Open Terminal",        "ctrl+alt+t" },
    { "browser",  "Open Surfer",          "super+b" },
    { "settings", "Open Settings",        "super+i" },
    { "activity", "Open Activity",        "ctrl+shift+esc" },
    { "close",    "Close the window",     "alt+f4" },
    { "minimize", "Minimize the window",  "super+down" },
    { "maximize", "Maximize the window",  "super+up" },
    { "switch",   "Switch windows",       "alt+tab" },
    { "desktop",  "Show the desktop",     "super+d" },
    { "overview", "Window overview",      "super+tab" },
    { "wifi",     "Wi-Fi",                "super+w" },
};
#define SC_COUNT ((int)(sizeof(sc_actions) / sizeof(sc_actions[0])))

typedef struct {
    uint8_t mods[SC_COUNT];
    uint8_t key[SC_COUNT];
    uint8_t set[SC_COUNT];
} sc_bindings_t;

static __attribute__((unused)) int sc_streq_n(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        if (x != b[i] || !b[i]) return 0;
    }
    return b[n] == 0;
}

static const struct { const char *name; uint8_t key; } sc_key_names[] = {
    { "tab", SC_KEY_TAB }, { "enter", SC_KEY_ENTER }, { "esc", SC_KEY_ESC }, { "space", SC_KEY_SPACE },
    { "up", SC_KEY_UP }, { "down", SC_KEY_DOWN }, { "left", SC_KEY_LEFT }, { "right", SC_KEY_RIGHT },
    { "delete", SC_KEY_DELETE },
};

/* "ctrl+alt+t" -> mods, key; 0 on success */
static __attribute__((unused)) int sc_parse(const char *s, int len, uint8_t *mods, uint8_t *key) {
    int i = 0;
    *mods = 0;
    *key = SC_KEY_NONE;
    while (i < len) {
        int j = i;
        while (j < len && s[j] != '+') j++;
        {
            const char *w = s + i;
            int n = j - i;
            if (sc_streq_n(w, "ctrl", n)) *mods |= SC_MOD_CTRL;
            else if (sc_streq_n(w, "alt", n)) *mods |= SC_MOD_ALT;
            else if (sc_streq_n(w, "shift", n)) *mods |= SC_MOD_SHIFT;
            else if (sc_streq_n(w, "super", n) || sc_streq_n(w, "win", n)) *mods |= SC_MOD_SUPER;
            else if (n >= 2 && (w[0] == 'f' || w[0] == 'F') && w[1] >= '1' && w[1] <= '9') {
                int f = w[1] - '0';
                if (n == 3 && w[2] >= '0' && w[2] <= '2') f = f * 10 + (w[2] - '0');
                if (f < 1 || f > 12) return -1;
                *key = (uint8_t)(SC_KEY_F1 + f - 1);
            } else if (n == 1) {
                char c = w[0];
                *key = (uint8_t)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            } else {
                int found = 0;
                for (unsigned k = 0; k < sizeof(sc_key_names) / sizeof(sc_key_names[0]); k++)
                    if (sc_streq_n(w, sc_key_names[k].name, n)) {
                        *key = sc_key_names[k].key;
                        found = 1;
                    }
                if (!found) return -1;
            }
        }
        i = j + 1;
    }
    return 0;
}

/* mods, key -> "Ctrl+Alt+T" (display) */
static __attribute__((unused)) void sc_format(uint8_t mods, uint8_t key, char *out, int cap) {
    int o = 0;
    const char *parts[4];
    int np = 0;
    char last[8];
    if (mods & SC_MOD_SUPER) parts[np++] = "Win";
    if (mods & SC_MOD_CTRL) parts[np++] = "Ctrl";
    if (mods & SC_MOD_ALT) parts[np++] = "Alt";
    if (mods & SC_MOD_SHIFT) parts[np++] = "Shift";
    last[0] = 0;
    if (key >= SC_KEY_F1 && key < SC_KEY_F1 + 12) {
        int f = key - SC_KEY_F1 + 1;
        last[0] = 'F';
        if (f >= 10) { last[1] = '1'; last[2] = (char)('0' + f - 10); last[3] = 0; }
        else { last[1] = (char)('0' + f); last[2] = 0; }
    } else if (key != SC_KEY_NONE) {
        const char *nm = 0;
        static const char *const pretty[] = { "Tab", "Enter", "Esc", "Space", "Up", "Down", "Left", "Right", "Delete" };
        for (unsigned k = 0; k < sizeof(sc_key_names) / sizeof(sc_key_names[0]); k++)
            if (sc_key_names[k].key == key) nm = pretty[k];
        if (nm) {
            int i = 0;
            for (; nm[i] && i < 7; i++) last[i] = nm[i];
            last[i] = 0;
        } else {
            last[0] = (char)(key >= 'a' && key <= 'z' ? key - 32 : key);
            last[1] = 0;
        }
    }
    out[0] = 0;
    for (int p = 0; p < np; p++) {
        for (const char *q = parts[p]; *q && o + 1 < cap; q++) out[o++] = *q;
        if ((p + 1 < np || last[0]) && o + 1 < cap) out[o++] = '+';
    }
    for (const char *q = last; *q && o + 1 < cap; q++) out[o++] = *q;
    out[o] = 0;
}

/* lower-case config form of a combination */
static __attribute__((unused)) void sc_format_config(uint8_t mods, uint8_t key, char *out, int cap) {
    char pretty[32];
    int o = 0;
    sc_format(mods, key, pretty, sizeof(pretty));
    for (int i = 0; pretty[i] && o + 1 < cap; i++) {
        char c = pretty[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[o++] = c;
    }
    out[o] = 0;
    /* "win" is spelled "super" in the file */
    if (o >= 3 && out[0] == 'w' && out[1] == 'i' && out[2] == 'n') {
        char tmp[40];
        int t = 0;
        const char *s = "super";
        for (; *s; s++) tmp[t++] = *s;
        for (int i = 3; out[i] && t + 1 < (int)sizeof(tmp); i++) tmp[t++] = out[i];
        tmp[t] = 0;
        for (t = 0; tmp[t] && t + 1 < cap; t++) out[t] = tmp[t];
        out[t] = 0;
    }
}

static __attribute__((unused)) void sc_load(sc_bindings_t *b) {
    char buf[2048];
    long n = (long)icda_read_file(SC_PATH, buf, sizeof(buf) - 1);
    for (int a = 0; a < SC_COUNT; a++) {
        const char *d = sc_actions[a].def;
        int len = 0;
        while (d[len]) len++;
        b->set[a] = sc_parse(d, len, &b->mods[a], &b->key[a]) == 0;
    }
    if (n <= 0) return;
    buf[n] = 0;
    for (long i = 0; i < n; ) {
        long s = i, eq = -1, e;
        while (i < n && buf[i] != '\n') {
            if (buf[i] == '=' && eq < 0) eq = i;
            i++;
        }
        e = i;
        if (i < n) i++;
        if (eq < 0) continue;
        for (int a = 0; a < SC_COUNT; a++) {
            if (sc_streq_n(buf + s, sc_actions[a].id, (int)(eq - s))) {
                uint8_t m, k;
                if (eq + 1 == e) b->set[a] = 0;      /* "action=" turns it off */
                else if (sc_parse(buf + eq + 1, (int)(e - eq - 1), &m, &k) == 0) {
                    b->mods[a] = m;
                    b->key[a] = k;
                    b->set[a] = 1;
                }
            }
        }
    }
}

static __attribute__((unused)) int sc_save(const sc_bindings_t *b) {
    char buf[2048];
    int o = 0;
    (void)icda_mkdir("/cfg");
    for (int a = 0; a < SC_COUNT; a++) {
        char combo[40];
        const char *id = sc_actions[a].id;
        while (*id && o + 1 < (int)sizeof(buf)) buf[o++] = *id++;
        if (o + 1 < (int)sizeof(buf)) buf[o++] = '=';
        if (b->set[a]) {
            sc_format_config(b->mods[a], b->key[a], combo, sizeof(combo));
            for (int i = 0; combo[i] && o + 1 < (int)sizeof(buf); i++) buf[o++] = combo[i];
        }
        if (o + 1 < (int)sizeof(buf)) buf[o++] = '\n';
    }
    return icda_write_file(SC_PATH, buf, (uint64_t)o) == (uint64_t)-1 ? -1 : 0;
}

/* the action bound to a chord, or -1 */
static __attribute__((unused)) int sc_match(const sc_bindings_t *b, uint8_t mods, uint8_t key) {
    for (int a = 0; a < SC_COUNT; a++)
        if (b->set[a] && b->mods[a] == mods && b->key[a] == key) return a;
    return -1;
}

#endif
