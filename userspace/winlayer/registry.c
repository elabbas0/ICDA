/* The registry: keys and values, shared by every Windows program of a
 * user, kept in <C: drive>/Windows/registry.txt.
 *
 * Programs that start each other (an installer and its setup) use the
 * registry together, so each change is made under a file lock on the file
 * as it is then (read again if another process changed it) and written
 * back at once.  An open key is its path, not a pointer: the tree may be
 * read again under it.  HKEY_CLASSES_ROOT is HKLM\Software\Classes.
 *
 * The file has a line per key, "[root\path]", then a line per value,
 * "name=type:data" with the name and data in hex, so any name and any data
 * fit. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>

#define REG_NONE      0
#define REG_SZ        1
#define REG_EXPAND_SZ 2
#define REG_BINARY    3
#define REG_DWORD     4
#define REG_MULTI_SZ  7
#define REG_QWORD     11

typedef struct rvalue {
    struct rvalue *next;
    WCHAR         *name;     /* "" is the default value */
    DWORD          type, size;
    uint8_t       *data;
} rvalue_t;

typedef struct rkey {
    struct rkey   *next, *children, *parent;
    WCHAR         *name;
    rvalue_t      *values;
    FILETIME       written;
} rkey_t;

static const char *const root_names[] = { "HKEY_LOCAL_MACHINE", "HKEY_CURRENT_USER", "HKEY_USERS", "HKEY_CURRENT_CONFIG" };
static rkey_t roots[4];
static pthread_mutex_t rlock;
static char reg_path[1100], lock_path[1100];
static struct timespec loaded_mtime;
static int loaded;

__attribute__((constructor)) static void rlock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&rlock, &a);
}

/* ---- the tree --------------------------------------------------------------------------- */

static WCHAR *wdup_n(const WCHAR *s, size_t n) {
    WCHAR *d = malloc((n + 1) * sizeof(WCHAR));
    memcpy(d, s, n * sizeof(WCHAR));
    d[n] = 0;
    return d;
}

static int name_eq(const WCHAR *a, const WCHAR *b, size_t bn) {
    size_t i = 0;
    for (; i < bn && a[i]; i++) {
        WCHAR x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return i == bn && !a[i];
}

static rkey_t *child(rkey_t *k, const WCHAR *name, size_t n, int make) {
    rkey_t *c;
    for (c = k->children; c; c = c->next)
        if (name_eq(c->name, name, n)) return c;
    if (!make) return 0;
    c = calloc(1, sizeof(*c));
    c->name = wdup_n(name, n);
    c->parent = k;
    c->next = k->children;
    k->children = c;
    unix_to_filetime(time(0), 0, &c->written);
    return c;
}

/* "a\b\c" under k (make: missing keys are made) */
static rkey_t *walk(rkey_t *k, const WCHAR *path, int make) {
    const WCHAR *p = path;
    if (!path) return k;
    while (k && *p) {
        const WCHAR *e = p;
        while (*e && *e != '\\') e++;
        if (e > p) k = child(k, p, (size_t)(e - p), make);
        p = *e ? e + 1 : e;
    }
    return k;
}

static void free_values(rvalue_t *v) {
    while (v) {
        rvalue_t *n = v->next;
        free(v->name);
        free(v->data);
        free(v);
        v = n;
    }
}

static void free_tree(rkey_t *k) {
    rkey_t *c = k->children;
    while (c) {
        rkey_t *n = c->next;
        free_tree(c);
        free(c->name);
        free(c);
        c = n;
    }
    free_values(k->values);
    k->children = 0;
    k->values = 0;
}

static rvalue_t *value_find(rkey_t *k, const WCHAR *name) {
    const WCHAR *n = name ? name : (const WCHAR *)u"";
    for (rvalue_t *v = k->values; v; v = v->next)
        if (name_eq(v->name, n, w_len(n))) return v;
    return 0;
}

/* ---- the file ----------------------------------------------------------------------------- */

static void put_hex(FILE *f, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]);
}

static void save_key(FILE *f, rkey_t *k, char *path, size_t len) {
    if (k->values || !k->children) {
        fprintf(f, "[%s]\n", path);
        for (rvalue_t *v = k->values; v; v = v->next) {
            put_hex(f, (const uint8_t *)v->name, w_len(v->name) * 2);
            fprintf(f, "=%u:", v->type);
            put_hex(f, v->data, v->size);
            fputc('\n', f);
        }
    }
    for (rkey_t *c = k->children; c; c = c->next) {
        char name[600];
        size_t n;
        w_to_utf8(c->name, -1, name, sizeof(name));
        n = strlen(name);
        if (len + n + 2 >= 8192) continue;
        path[len] = '\\';
        memcpy(path + len + 1, name, n + 1);
        save_key(f, c, path, len + 1 + n);
        path[len] = 0;
    }
}

static void reg_save(void) {
    char tmp[1200], path[8192];
    FILE *f;
    struct stat st;
    snprintf(tmp, sizeof(tmp), "%s.%d", reg_path, getpid());
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "ICDA registry 1\n");
    for (int r = 0; r < 4; r++) {
        snprintf(path, sizeof(path), "%s", root_names[r]);
        save_key(f, &roots[r], path, strlen(path));
    }
    fclose(f);
    rename(tmp, reg_path);
    if (stat(reg_path, &st) == 0) loaded_mtime = st.st_mtim;
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static size_t from_hex(const char *s, size_t n, uint8_t *out) {
    size_t o = 0;
    for (size_t i = 0; i + 1 < n + 1 && i + 1 <= n - 1 + 1 && i + 1 < n + 1; i += 2) {
        int h, l;
        if (i + 1 >= n) break;
        h = hexv(s[i]), l = hexv(s[i + 1]);
        if (h < 0 || l < 0) break;
        out[o++] = (uint8_t)(h << 4 | l);
    }
    return o;
}

static void reg_load(void) {
    FILE *f;
    char *line = 0;
    size_t cap = 0;
    ssize_t n;
    rkey_t *cur = 0;
    struct stat st;
    for (int r = 0; r < 4; r++) free_tree(&roots[r]);
    f = fopen(reg_path, "r");
    if (!f) {
        loaded_mtime.tv_sec = 0;
        loaded_mtime.tv_nsec = 0;
        return;
    }
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n') line[--n] = 0;
        if (line[0] == '[') {
            char *e = strrchr(line, ']'), *bs;
            WCHAR *w;
            int r;
            if (!e) continue;
            *e = 0;
            bs = strchr(line + 1, '\\');
            if (bs) *bs = 0;
            for (r = 0; r < 4 && strcmp(line + 1, root_names[r]); r++) {
            }
            if (r == 4) {
                cur = 0;
                continue;
            }
            w = utf8_to_w_dup(bs ? bs + 1 : "");
            cur = walk(&roots[r], w, 1);
            free(w);
        } else if (cur && strchr(line, '=')) {
            char *eq = strchr(line, '='), *colon = strchr(eq, ':');
            rvalue_t *v;
            size_t nl;
            if (!colon) continue;
            v = calloc(1, sizeof(*v));
            nl = (size_t)(eq - line) / 2;
            v->name = calloc(nl / 2 + 1, sizeof(WCHAR));
            from_hex(line, (size_t)(eq - line), (uint8_t *)v->name);
            v->type = (DWORD)strtoul(eq + 1, 0, 10);
            v->size = (DWORD)(strlen(colon + 1) / 2);
            v->data = malloc(v->size + 2);
            from_hex(colon + 1, strlen(colon + 1), v->data);
            v->next = cur->values;
            cur->values = v;
        }
    }
    free(line);
    fclose(f);
    if (stat(reg_path, &st) == 0) loaded_mtime = st.st_mtim;
}

/* the tree as the file has it now (another program may have changed it) */
static void reg_fresh(void) {
    struct stat st;
    if (!reg_path[0]) {
        snprintf(reg_path, sizeof(reg_path), "%s/Windows/registry.txt", path_prefix());
        snprintf(lock_path, sizeof(lock_path), "%s/Windows/registry.lock", path_prefix());
    }
    if (!loaded || (stat(reg_path, &st) == 0 &&
                    (st.st_mtim.tv_sec != loaded_mtime.tv_sec || st.st_mtim.tv_nsec != loaded_mtime.tv_nsec))) {
        reg_load();
        loaded = 1;
    }
}

static int lock_fd = -1;

/* a change: under the file lock, on the tree as it is, written back */
static void change_begin(void) {
    pthread_mutex_lock(&rlock);
    reg_fresh();
    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd >= 0) flock(lock_fd, LOCK_EX);
    loaded = 0;
    reg_fresh();
}

static void change_end(int changed) {
    if (changed) reg_save();
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        lock_fd = -1;
    }
    pthread_mutex_unlock(&rlock);
}

/* ---- keys as handles ------------------------------------------------------------------------ */

/* the predefined keys are LONGs made pointers: sign-extended on x64 */
#define HKCR ((HKEY)(intptr_t)(int32_t)0x80000000)
#define HKCU ((HKEY)(intptr_t)(int32_t)0x80000001)
#define HKLM ((HKEY)(intptr_t)(int32_t)0x80000002)
#define HKU  ((HKEY)(intptr_t)(int32_t)0x80000003)
#define HKCC ((HKEY)(intptr_t)(int32_t)0x80000005)

static uintptr_t predef(HKEY h) {
    uintptr_t v = (uintptr_t)h;
    if ((v >> 32) == 0xFFFFFFFFu) v &= 0xFFFFFFFFu;
    return (v >= 0x80000000u && v <= 0x80000006u) ? v : 0;
}

typedef struct {
    int    magic;
    int    root;          /* 0 HKLM, 1 HKCU, 2 HKU, 3 HKCC */
    WCHAR *path;
} rhandle_t;

#define RH_MAGIC 0x52454721

/* a key's root and full path */
static int key_where(HKEY h, int *root, WCHAR **path) {
    uintptr_t v = predef(h);
    if (v) {
        static const WCHAR classes[] = u"Software\\Classes";
        switch (v) {
        case 0x80000000: *root = 0, *path = w_dup(classes); return 0;
        case 0x80000001: *root = 1, *path = w_dup((const WCHAR *)u""); return 0;
        case 0x80000002: *root = 0, *path = w_dup((const WCHAR *)u""); return 0;
        case 0x80000003: *root = 2, *path = w_dup((const WCHAR *)u""); return 0;
        case 0x80000005: *root = 3, *path = w_dup((const WCHAR *)u""); return 0;
        default: return -1;
        }
    }
    {
        whandle_t *w = h_get(h, H_OTHER);
        rhandle_t *r = w ? w->obj : 0;
        if (!r || r->magic != RH_MAGIC) return -1;
        *root = r->root;
        *path = w_dup(r->path);
        return 0;
    }
}

static WCHAR *join(const WCHAR *a, const WCHAR *b) {
    size_t na = w_len(a), nb = b ? w_len(b) : 0;
    WCHAR *r;
    while (nb && b[0] == '\\') b++, nb--;
    r = malloc((na + nb + 2) * sizeof(WCHAR));
    memcpy(r, a, na * 2);
    if (na && nb) r[na++] = '\\';
    if (nb) memcpy(r + na, b, nb * 2);
    r[na + nb] = 0;
    return r;
}

static HKEY new_handle(int root, WCHAR *path) {
    rhandle_t *r = calloc(1, sizeof(*r));
    r->magic = RH_MAGIC;
    r->root = root;
    r->path = path;
    return h_new(H_OTHER, r);
}

/* the key a handle and subkey name (0 if missing) */
static rkey_t *resolve(HKEY h, const WCHAR *sub, int make, int *root_out, WCHAR **path_out) {
    int root;
    WCHAR *base, *full;
    rkey_t *k;
    if (path_out) *path_out = 0;
    if (key_where(h, &root, &base) != 0) return 0;
    full = join(base, sub);
    free(base);
    k = walk(&roots[root], full, make);
    if (root_out) *root_out = root;
    if (path_out) *path_out = full;
    else free(full);
    return k;
}

static LONG open_key(HKEY h, const WCHAR *sub, HKEY *out, int make, DWORD *disp) {
    int root = 0;
    WCHAR *path = 0;
    rkey_t *k;
    if (make) change_begin();
    else {
        pthread_mutex_lock(&rlock);
        reg_fresh();
    }
    {
        int existed = !make || resolve(h, sub, 0, 0, 0) != 0;
        if (disp) *disp = existed ? 2 : 1;      /* REG_OPENED_EXISTING_KEY, REG_CREATED_NEW_KEY */
        k = resolve(h, sub, make, &root, &path);
        if (make) change_end(!existed && k);
        else pthread_mutex_unlock(&rlock);
    }
    if (!k) {
        free(path);
        path = 0;
        if (out) *out = 0;
        return key_where(h, &root, &path) == 0 ? (free(path), ERROR_FILE_NOT_FOUND) : ERROR_INVALID_HANDLE;
    }
    *out = new_handle(root, path);
    return 0;
}

LONG WINAPI RegOpenKeyExW(HKEY h, const WCHAR *sub, DWORD opt, DWORD sam, HKEY *out) {
    (void)opt, (void)sam;
    return open_key(h, sub, out, 0, 0);
}

LONG WINAPI RegOpenKeyExA(HKEY h, const char *sub, DWORD opt, DWORD sam, HKEY *out) {
    WCHAR *w = sub ? utf8_to_w_dup(sub) : 0;
    LONG r;
    (void)opt, (void)sam;
    r = open_key(h, w, out, 0, 0);
    free(w);
    return r;
}

LONG WINAPI RegOpenKeyW(HKEY h, const WCHAR *sub, HKEY *out) { return open_key(h, sub, out, 0, 0); }
LONG WINAPI RegOpenKeyA(HKEY h, const char *sub, HKEY *out) { return RegOpenKeyExA(h, sub, 0, 0, out); }

LONG WINAPI RegCreateKeyExW(HKEY h, const WCHAR *sub, DWORD r, WCHAR *cls, DWORD opt, DWORD sam, void *sa, HKEY *out, DWORD *disp) {
    (void)r, (void)cls, (void)opt, (void)sam, (void)sa;
    return open_key(h, sub, out, 1, disp);
}

LONG WINAPI RegCreateKeyExA(HKEY h, const char *sub, DWORD r, char *cls, DWORD opt, DWORD sam, void *sa, HKEY *out, DWORD *disp) {
    WCHAR *w = sub ? utf8_to_w_dup(sub) : 0;
    LONG res;
    (void)r, (void)cls, (void)opt, (void)sam, (void)sa;
    res = open_key(h, w, out, 1, disp);
    free(w);
    return res;
}

LONG WINAPI RegCreateKeyW(HKEY h, const WCHAR *sub, HKEY *out) { return open_key(h, sub, out, 1, 0); }
LONG WINAPI RegCreateKeyA(HKEY h, const char *sub, HKEY *out) { return RegCreateKeyExA(h, sub, 0, 0, 0, 0, 0, out, 0); }

LONG WINAPI RegCloseKey(HKEY h) {
    whandle_t *w = h_get(h, H_OTHER);
    rhandle_t *r = w ? w->obj : 0;
    if (predef(h)) return 0;
    if (!r || r->magic != RH_MAGIC) return ERROR_INVALID_HANDLE;
    r->magic = 0;
    free(r->path);
    free(r);
    h_close(h);
    return 0;
}

LONG WINAPI RegOpenCurrentUser(DWORD sam, HKEY *out) {
    (void)sam;
    *out = HKCU;
    return 0;
}

LONG WINAPI RegOverridePredefKey(HKEY h, HKEY n) {
    (void)h, (void)n;
    return 0;
}

LONG WINAPI RegFlushKey(HKEY h) {
    (void)h;
    return 0;
}

/* ---- values -------------------------------------------------------------------------------- */

/* the value's data as asked (sz strings get their NUL) */
static LONG give(const rvalue_t *v, DWORD *type, BYTE *data, DWORD *size) {
    if (type) *type = v->type;
    if (!size) return data ? ERROR_INVALID_PARAMETER : 0;
    if (data) {
        if (*size < v->size) {
            *size = v->size;
            return ERROR_MORE_DATA;
        }
        memcpy(data, v->data, v->size);
    }
    *size = v->size;
    return 0;
}

LONG WINAPI RegQueryValueExW(HKEY h, const WCHAR *name, DWORD *res, DWORD *type, BYTE *data, DWORD *size) {
    rkey_t *k;
    rvalue_t *v;
    LONG r;
    (void)res;
    pthread_mutex_lock(&rlock);
    reg_fresh();
    k = resolve(h, 0, 0, 0, 0);
    v = k ? value_find(k, name) : 0;
    r = !k ? ERROR_INVALID_HANDLE : !v ? ERROR_FILE_NOT_FOUND : give(v, type, data, size);
    pthread_mutex_unlock(&rlock);
    return r;
}

/* the ANSI form: strings in UTF-8 */
static int is_string(DWORD t) { return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ; }

LONG WINAPI RegQueryValueExA(HKEY h, const char *name, DWORD *res, DWORD *type, BYTE *data, DWORD *size) {
    WCHAR *wn = name ? utf8_to_w_dup(name) : 0;
    DWORD t = 0, n = 0;
    LONG r;
    BYTE *buf;
    r = RegQueryValueExW(h, wn, res, &t, 0, &n);
    if (r) {
        free(wn);
        return r;
    }
    buf = malloc(n + 4);
    RegQueryValueExW(h, wn, res, &t, buf, &n);
    free(wn);
    if (type) *type = t;
    if (is_string(t)) {
        int need = w_to_utf8((const WCHAR *)buf, (int)(n / 2), 0, 0);
        char *a = malloc((size_t)need + 2);
        w_to_utf8((const WCHAR *)buf, (int)(n / 2), a, need + 2);
        free(buf);
        buf = (BYTE *)a;
        n = (DWORD)need - 1;
        if (n == 0 || a[n - 1]) n++;           /* with its NUL */
    }
    if (size) {
        if (data && *size < n) {
            *size = n;
            free(buf);
            return ERROR_MORE_DATA;
        }
        if (data) memcpy(data, buf, n);
        *size = n;
    }
    free(buf);
    return 0;
}

LONG WINAPI RegSetValueExW(HKEY h, const WCHAR *name, DWORD res, DWORD type, const BYTE *data, DWORD size) {
    rkey_t *k;
    rvalue_t *v;
    (void)res;
    change_begin();
    k = resolve(h, 0, 0, 0, 0);
    if (!k) {
        change_end(0);
        return ERROR_INVALID_HANDLE;
    }
    v = value_find(k, name);
    if (!v) {
        const WCHAR *n = name ? name : (const WCHAR *)u"";
        v = calloc(1, sizeof(*v));
        v->name = w_dup(n);
        v->next = k->values;
        k->values = v;
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = malloc(size + 2);
    if (size && data) memcpy(v->data, data, size);
    unix_to_filetime(time(0), 0, &k->written);
    change_end(1);
    return 0;
}

LONG WINAPI RegSetValueExA(HKEY h, const char *name, DWORD res, DWORD type, const BYTE *data, DWORD size) {
    WCHAR *wn = name ? utf8_to_w_dup(name) : 0;
    LONG r;
    if (is_string(type) && data) {
        /* "a\0b\0\0" for multi strings: each part converted */
        WCHAR *w = malloc(((size_t)size + 2) * sizeof(WCHAR));
        int n = 0;
        for (DWORD i = 0; i < size;) {
            const char *s = (const char *)data + i;
            size_t l = strnlen(s, size - i);
            n += utf8_to_w(s, (int)l, w + n, (int)(size + 2) - n) - 1;
            w[n++] = 0;
            i += (DWORD)l + 1;
        }
        r = RegSetValueExW(h, wn, res, type, (const BYTE *)w, (DWORD)n * 2);
        free(w);
    } else {
        r = RegSetValueExW(h, wn, res, type, data, size);
    }
    free(wn);
    return r;
}

LONG WINAPI RegSetKeyValueW(HKEY h, const WCHAR *sub, const WCHAR *name, DWORD type, const void *data, DWORD size) {
    HKEY k;
    LONG r = open_key(h, sub, &k, 1, 0);
    if (r) return r;
    r = RegSetValueExW(k, name, 0, type, data, size);
    RegCloseKey(k);
    return r;
}

/* RegGetValue: a value of a subkey, with the types allowed */
#define RRF_RT_ANY 0xFFFF
LONG WINAPI RegGetValueW(HKEY h, const WCHAR *sub, const WCHAR *name, DWORD flags, DWORD *type, void *data, DWORD *size) {
    HKEY k = h;
    DWORD t = 0, n = size ? *size : 0;
    LONG r;
    if (sub && *sub) {
        r = open_key(h, sub, &k, 0, 0);
        if (r) return r;
    }
    r = RegQueryValueExW(k, name, 0, &t, data, size ? &n : 0);
    if (k != h) RegCloseKey(k);
    if (r == 0 || r == ERROR_MORE_DATA) {
        DWORD allowed = flags & RRF_RT_ANY;
        int ok = allowed == RRF_RT_ANY || (t == REG_SZ && (allowed & 0x2)) || (t == REG_EXPAND_SZ && (allowed & 0x4)) ||
                 (t == REG_BINARY && (allowed & 0x8)) || (t == REG_DWORD && (allowed & 0x10)) ||
                 (t == REG_MULTI_SZ && (allowed & 0x20)) || (t == REG_QWORD && (allowed & 0x40)) ||
                 (t == REG_NONE && (allowed & 0x1));
        if (!ok) return 1630;   /* ERROR_UNSUPPORTED_TYPE */
    }
    if (type) *type = t;
    if (size) *size = n;
    return r;
}

LONG WINAPI RegGetValueA(HKEY h, const char *sub, const char *name, DWORD flags, DWORD *type, void *data, DWORD *size) {
    HKEY k = h;
    LONG r;
    if (sub && *sub) {
        r = RegOpenKeyExA(h, sub, 0, 0, &k);
        if (r) return r;
    }
    (void)flags;
    r = RegQueryValueExA(k, name, 0, type, data, size);
    if (k != h) RegCloseKey(k);
    return r;
}

LONG WINAPI RegDeleteValueW(HKEY h, const WCHAR *name) {
    rkey_t *k;
    rvalue_t **pp;
    change_begin();
    k = resolve(h, 0, 0, 0, 0);
    if (!k) {
        change_end(0);
        return ERROR_INVALID_HANDLE;
    }
    for (pp = &k->values; *pp; pp = &(*pp)->next) {
        const WCHAR *n = name ? name : (const WCHAR *)u"";
        if (name_eq((*pp)->name, n, w_len(n))) {
            rvalue_t *v = *pp;
            *pp = v->next;
            v->next = 0;
            free_values(v);
            change_end(1);
            return 0;
        }
    }
    change_end(0);
    return ERROR_FILE_NOT_FOUND;
}

LONG WINAPI RegDeleteValueA(HKEY h, const char *name) {
    WCHAR *w = name ? utf8_to_w_dup(name) : 0;
    LONG r = RegDeleteValueW(h, w);
    free(w);
    return r;
}

LONG WINAPI RegDeleteKeyValueW(HKEY h, const WCHAR *sub, const WCHAR *name) {
    HKEY k;
    LONG r = open_key(h, sub, &k, 0, 0);
    if (r) return r;
    r = RegDeleteValueW(k, name);
    RegCloseKey(k);
    return r;
}

/* deleting a key: only without subkeys (RegDeleteTree takes all) */
static LONG delete_key(HKEY h, const WCHAR *sub, int tree) {
    rkey_t *k, **pp;
    change_begin();
    k = resolve(h, sub, 0, 0, 0);
    if (!k || !k->parent) {
        change_end(0);
        return k ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND;
    }
    if (!tree && k->children) {
        change_end(0);
        return ERROR_ACCESS_DENIED;
    }
    for (pp = &k->parent->children; *pp; pp = &(*pp)->next)
        if (*pp == k) {
            *pp = k->next;
            break;
        }
    free_tree(k);
    free(k->name);
    free(k);
    change_end(1);
    return 0;
}

LONG WINAPI RegDeleteKeyW(HKEY h, const WCHAR *sub) { return delete_key(h, sub, 0); }
LONG WINAPI RegDeleteKeyExW(HKEY h, const WCHAR *sub, DWORD sam, DWORD r) {
    (void)sam, (void)r;
    return delete_key(h, sub, 0);
}
LONG WINAPI RegDeleteKeyA(HKEY h, const char *sub) {
    WCHAR *w = utf8_to_w_dup(sub ? sub : "");
    LONG r = delete_key(h, w, 0);
    free(w);
    return r;
}
LONG WINAPI RegDeleteTreeW(HKEY h, const WCHAR *sub) {
    if (!sub || !*sub) {
        /* everything under the key itself: its values and subkeys */
        rkey_t *k;
        change_begin();
        k = resolve(h, 0, 0, 0, 0);
        if (k) free_tree(k);
        change_end(k != 0);
        return k ? 0 : ERROR_INVALID_HANDLE;
    }
    return delete_key(h, sub, 1);
}

/* ---- enumerating -------------------------------------------------------------------------- */

LONG WINAPI RegEnumKeyExW(HKEY h, DWORD i, WCHAR *name, DWORD *nn, DWORD *r, WCHAR *cls, DWORD *cn, FILETIME *ft) {
    rkey_t *k, *c;
    DWORD idx = 0;
    LONG res = ERROR_NO_MORE_ITEMS;
    (void)r;
    pthread_mutex_lock(&rlock);
    reg_fresh();
    k = resolve(h, 0, 0, 0, 0);
    if (!k) res = ERROR_INVALID_HANDLE;
    for (c = k ? k->children : 0; c; c = c->next, idx++) {
        if (idx != i) continue;
        {
            size_t l = w_len(c->name);
            if (l + 1 > *nn) {
                *nn = (DWORD)l + 1;
                res = ERROR_MORE_DATA;
                break;
            }
            memcpy(name, c->name, (l + 1) * 2);
            *nn = (DWORD)l;
            if (cls && cn && *cn) cls[0] = 0;
            if (cn) *cn = 0;
            if (ft) *ft = c->written;
            res = 0;
        }
        break;
    }
    pthread_mutex_unlock(&rlock);
    return res;
}

LONG WINAPI RegEnumKeyW(HKEY h, DWORD i, WCHAR *name, DWORD n) { return RegEnumKeyExW(h, i, name, &n, 0, 0, 0, 0); }

LONG WINAPI RegEnumKeyExA(HKEY h, DWORD i, char *name, DWORD *nn, DWORD *r, char *cls, DWORD *cn, FILETIME *ft) {
    WCHAR w[512];
    DWORD wn = 512;
    LONG res = RegEnumKeyExW(h, i, w, &wn, r, 0, 0, ft);
    int need;
    if (res) return res;
    need = w_to_utf8(w, -1, 0, 0);
    if ((DWORD)need > *nn) {
        *nn = (DWORD)need;
        return ERROR_MORE_DATA;
    }
    w_to_utf8(w, -1, name, (int)*nn);
    *nn = (DWORD)need - 1;
    if (cls && cn && *cn) cls[0] = 0;
    if (cn) *cn = 0;
    return 0;
}

LONG WINAPI RegEnumValueW(HKEY h, DWORD i, WCHAR *name, DWORD *nn, DWORD *r, DWORD *type, BYTE *data, DWORD *dn) {
    rkey_t *k;
    rvalue_t *v;
    DWORD idx = 0;
    LONG res = ERROR_NO_MORE_ITEMS;
    (void)r;
    pthread_mutex_lock(&rlock);
    reg_fresh();
    k = resolve(h, 0, 0, 0, 0);
    if (!k) res = ERROR_INVALID_HANDLE;
    for (v = k ? k->values : 0; v; v = v->next, idx++) {
        size_t l;
        if (idx != i) continue;
        l = w_len(v->name);
        if (l + 1 > *nn) {
            *nn = (DWORD)l + 1;
            res = ERROR_MORE_DATA;
            break;
        }
        memcpy(name, v->name, (l + 1) * 2);
        *nn = (DWORD)l;
        res = give(v, type, data, dn);
        break;
    }
    pthread_mutex_unlock(&rlock);
    return res;
}

LONG WINAPI RegEnumValueA(HKEY h, DWORD i, char *name, DWORD *nn, DWORD *r, DWORD *type, BYTE *data, DWORD *dn) {
    WCHAR w[512];
    DWORD wn = 512, t = 0, n = 0;
    LONG res = RegEnumValueW(h, i, w, &wn, r, &t, 0, &n);
    if (res && res != ERROR_MORE_DATA) return res;
    w_to_utf8(w, -1, name, (int)*nn);
    *nn = (DWORD)strlen(name);
    if (type) *type = t;
    return RegQueryValueExA(h, name, 0, 0, data, dn);
}

LONG WINAPI RegQueryInfoKeyW(HKEY h, WCHAR *cls, DWORD *cn, DWORD *r, DWORD *nsub, DWORD *maxsub, DWORD *maxcls, DWORD *nval,
                             DWORD *maxvn, DWORD *maxvl, DWORD *sd, FILETIME *ft) {
    rkey_t *k;
    DWORD ns = 0, ms = 0, nv = 0, mvn = 0, mvl = 0;
    (void)r;
    pthread_mutex_lock(&rlock);
    reg_fresh();
    k = resolve(h, 0, 0, 0, 0);
    if (!k) {
        pthread_mutex_unlock(&rlock);
        return ERROR_INVALID_HANDLE;
    }
    for (rkey_t *c = k->children; c; c = c->next) {
        ns++;
        if (w_len(c->name) > ms) ms = (DWORD)w_len(c->name);
    }
    for (rvalue_t *v = k->values; v; v = v->next) {
        nv++;
        if (w_len(v->name) > mvn) mvn = (DWORD)w_len(v->name);
        if (v->size > mvl) mvl = v->size;
    }
    if (cls && cn && *cn) cls[0] = 0;
    if (cn) *cn = 0;
    if (nsub) *nsub = ns;
    if (maxsub) *maxsub = ms;
    if (maxcls) *maxcls = 0;
    if (nval) *nval = nv;
    if (maxvn) *maxvn = mvn;
    if (maxvl) *maxvl = mvl;
    if (sd) *sd = 0;
    if (ft) *ft = k->written;
    pthread_mutex_unlock(&rlock);
    return 0;
}

LONG WINAPI RegQueryInfoKeyA(HKEY h, char *cls, DWORD *cn, DWORD *r, DWORD *nsub, DWORD *maxsub, DWORD *maxcls, DWORD *nval,
                             DWORD *maxvn, DWORD *maxvl, DWORD *sd, FILETIME *ft) {
    return RegQueryInfoKeyW(h, (WCHAR *)cls, cn, r, nsub, maxsub, maxcls, nval, maxvn, maxvl, sd, ft);
}

LONG WINAPI RegNotifyChangeKeyValue(HKEY h, BOOL sub, DWORD f, HANDLE ev, BOOL async) {
    (void)h, (void)sub, (void)f, (void)ev, (void)async;
    return 0;
}

LONG WINAPI RegCopyTreeW(HKEY from, const WCHAR *sub, HKEY to) {
    (void)from, (void)sub, (void)to;
    return ERROR_CALL_NOT_IMPLEMENTED;
}

LONG WINAPI RegLoadKeyW(HKEY h, const WCHAR *sub, const WCHAR *file) {
    (void)h, (void)sub, (void)file;
    return ERROR_ACCESS_DENIED;
}

LONG WINAPI RegUnLoadKeyW(HKEY h, const WCHAR *sub) {
    (void)h, (void)sub;
    return ERROR_ACCESS_DENIED;
}

/* ---- ntdll's view ------------------------------------------------------------------------- */

typedef struct {
    USHORT Length, MaximumLength;
    WCHAR *Buffer;
} UNICODE_STRING;

/* \Registry\User\<the user's SID>: what HKEY_CURRENT_USER is */
LONG WINAPI RtlFormatCurrentUserKeyPath(UNICODE_STRING *out) {
    static const char path[] = "\\Registry\\User\\S-1-5-21-1001-1002-1003-1000";
    size_t n = sizeof(path) - 1;
    out->Buffer = malloc((n + 1) * 2);
    utf8_to_w(path, -1, out->Buffer, (int)n + 1);
    out->Length = (USHORT)(n * 2);
    out->MaximumLength = (USHORT)((n + 1) * 2);
    return 0;
}

void WINAPI RtlInitUnicodeString(UNICODE_STRING *s, const WCHAR *src) {
    size_t n = src ? w_len(src) : 0;
    s->Buffer = (WCHAR *)src;
    s->Length = (USHORT)(n * 2);
    s->MaximumLength = (USHORT)(src ? n * 2 + 2 : 0);
}

void WINAPI RtlFreeUnicodeString(UNICODE_STRING *s) {
    free(s->Buffer);
    s->Buffer = 0;
    s->Length = s->MaximumLength = 0;
}

/* exported to ntdll by kernel32.c's table: the native forms of the above */
typedef struct {
    ULONG           Length;
    HANDLE          RootDirectory;
    UNICODE_STRING *ObjectName;
    ULONG           Attributes;
    void           *SecurityDescriptor, *SecurityQualityOfService;
} OBJECT_ATTRIBUTES;

/* a native path (\Registry\Machine\..., \Registry\User\<sid>\...) to a root and path */
static HKEY native_root(const WCHAR *p, const WCHAR **rest) {
    static const struct {
        const char *prefix;
        HKEY        root;
    } map[] = { { "\\Registry\\Machine", HKLM }, { "\\Registry\\User\\S-1-5-21-1001-1002-1003-1000", HKCU }, { "\\Registry\\User", HKU } };
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        size_t n = strlen(map[i].prefix), k;
        for (k = 0; k < n && p[k]; k++) {
            WCHAR a = p[k], b = (WCHAR)map[i].prefix[k];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
        }
        if (k == n && (p[n] == 0 || p[n] == '\\')) {
            *rest = p[n] ? p + n + 1 : p + n;
            return map[i].root;
        }
    }
    return 0;
}

LONG WINAPI NtOpenKeyEx(HKEY *out, DWORD access, OBJECT_ATTRIBUTES *oa, ULONG opts) {
    WCHAR *name;
    const WCHAR *rest;
    HKEY root;
    LONG r;
    (void)access, (void)opts;
    if (!oa || !oa->ObjectName) return (LONG)0xC000000D;      /* STATUS_INVALID_PARAMETER */
    name = calloc((size_t)oa->ObjectName->Length / 2 + 1, sizeof(WCHAR));
    memcpy(name, oa->ObjectName->Buffer, oa->ObjectName->Length);
    if (oa->RootDirectory) {
        r = RegOpenKeyExW(oa->RootDirectory, name, 0, 0, out);
    } else {
        root = native_root(name, &rest);
        r = root ? RegOpenKeyExW(root, rest, 0, 0, out) : ERROR_FILE_NOT_FOUND;
    }
    free(name);
    return r == 0 ? 0 : r == ERROR_FILE_NOT_FOUND ? (LONG)0xC0000034 : (LONG)0xC0000022;   /* NOT_FOUND, ACCESS_DENIED */
}

LONG WINAPI NtOpenKey(HKEY *out, DWORD access, OBJECT_ATTRIBUTES *oa) { return NtOpenKeyEx(out, access, oa, 0); }

static WCHAR *us_dup(const UNICODE_STRING *u) {
    WCHAR *n = calloc((size_t)(u ? u->Length / 2 : 0) + 1, sizeof(WCHAR));
    if (n && u && u->Buffer) memcpy(n, u->Buffer, u->Length);
    return n;
}

LONG WINAPI NtCreateKey(HKEY *out, DWORD access, OBJECT_ATTRIBUTES *oa, ULONG title, UNICODE_STRING *cls, ULONG opts, ULONG *disp) {
    WCHAR *name;
    const WCHAR *rest;
    HKEY root;
    LONG r;
    DWORD d = 0;
    (void)title, (void)cls;
    if (!oa || !oa->ObjectName) return (LONG)0xC000000D;
    name = us_dup(oa->ObjectName);
    if (oa->RootDirectory) {
        r = RegCreateKeyExW(oa->RootDirectory, name, 0, 0, opts, access, 0, out, &d);
    } else {
        root = native_root(name, &rest);
        r = root ? RegCreateKeyExW(root, rest, 0, 0, opts, access, 0, out, &d) : ERROR_FILE_NOT_FOUND;
    }
    free(name);
    if (disp) *disp = d;
    return r == 0 ? 0 : r == ERROR_FILE_NOT_FOUND ? (LONG)0xC0000034 : (LONG)0xC0000022;
}

LONG WINAPI NtSetValueKey(HKEY h, UNICODE_STRING *name, ULONG title, ULONG type, const void *data, ULONG size) {
    WCHAR *n = us_dup(name);
    LONG r = RegSetValueExW(h, n, 0, type, data, size);
    (void)title;
    free(n);
    return r == 0 ? 0 : (LONG)0xC0000022;
}

LONG WINAPI NtDeleteValueKey(HKEY h, UNICODE_STRING *name) {
    WCHAR *n = us_dup(name);
    LONG r = RegDeleteValueW(h, n);
    free(n);
    return r == 0 ? 0 : (LONG)0xC0000034;
}

LONG WINAPI NtDeleteKey(HKEY h) { return RegDeleteTreeW(h, 0) == 0 ? 0 : (LONG)0xC0000034; }

/* KEY_VALUE_PARTIAL_INFORMATION (class 2) is what programs ask */
LONG WINAPI NtQueryValueKey(HKEY h, UNICODE_STRING *name, int cls, void *info, ULONG len, ULONG *ret) {
    WCHAR *n = calloc((size_t)(name ? name->Length / 2 : 0) + 1, sizeof(WCHAR));
    DWORD type = 0, size = 0;
    LONG r;
    if (name) memcpy(n, name->Buffer, name->Length);
    r = RegQueryValueExW(h, n, 0, &type, 0, &size);
    if (r) {
        free(n);
        return (LONG)0xC0000034;
    }
    if (cls == 2) {
        ULONG need = 12 + size;
        if (ret) *ret = need;
        if (len < need) {
            free(n);
            return (LONG)0xC0000023;      /* STATUS_BUFFER_TOO_SMALL */
        }
        ((ULONG *)info)[0] = 0;
        ((ULONG *)info)[1] = type;
        ((ULONG *)info)[2] = size;
        RegQueryValueExW(h, n, 0, 0, (BYTE *)info + 12, &size);
        free(n);
        return 0;
    }
    free(n);
    return (LONG)0xC0000003;              /* STATUS_INVALID_INFO_CLASS */
}

LONG WINAPI NtClose(HANDLE h) {
    whandle_t *w = h_get(h, H_OTHER);
    if (w && w->obj && ((rhandle_t *)w->obj)->magic == RH_MAGIC) return RegCloseKey(h);
    {
        BOOL WINAPI CloseHandle(HANDLE h);
        return CloseHandle(h) ? 0 : (LONG)0xC0000008;      /* STATUS_INVALID_HANDLE */
    }
}

#define X(f) WEXP(f)
const wexport_t reg_exports[] = {
    X(RegOpenKeyExW), X(RegOpenKeyExA), X(RegOpenKeyW), X(RegOpenKeyA), X(RegCreateKeyExW), X(RegCreateKeyExA),
    X(RegCreateKeyW), X(RegCreateKeyA), X(RegCloseKey), X(RegOpenCurrentUser), X(RegOverridePredefKey), X(RegFlushKey),
    X(RegQueryValueExW), X(RegQueryValueExA), X(RegSetValueExW), X(RegSetValueExA), X(RegSetKeyValueW), X(RegGetValueW),
    X(RegGetValueA), X(RegDeleteValueW), X(RegDeleteValueA), X(RegDeleteKeyValueW), X(RegDeleteKeyW), X(RegDeleteKeyExW),
    X(RegDeleteKeyA), X(RegDeleteTreeW), X(RegEnumKeyExW), X(RegEnumKeyW), X(RegEnumKeyExA), X(RegEnumValueW),
    X(RegEnumValueA), X(RegQueryInfoKeyW), X(RegQueryInfoKeyA), X(RegNotifyChangeKeyValue), X(RegCopyTreeW),
    X(RegLoadKeyW), X(RegUnLoadKeyW),
};
const int reg_nexports = sizeof(reg_exports) / sizeof(reg_exports[0]);

const wexport_t reg_nt_exports[] = {
    X(RtlFormatCurrentUserKeyPath), X(RtlInitUnicodeString), X(RtlFreeUnicodeString), X(NtOpenKeyEx), X(NtOpenKey),
    X(NtDeleteKey), X(NtQueryValueKey), X(NtClose), WEXPN("ZwClose", NtClose), WEXPN("ZwOpenKeyEx", NtOpenKeyEx),
    WEXPN("ZwQueryValueKey", NtQueryValueKey), X(NtCreateKey), X(NtSetValueKey), X(NtDeleteValueKey),
    WEXPN("ZwCreateKey", NtCreateKey), WEXPN("ZwSetValueKey", NtSetValueKey), WEXPN("ZwDeleteValueKey", NtDeleteValueKey),
    WEXPN("ZwOpenKey", NtOpenKey), WEXPN("ZwDeleteKey", NtDeleteKey),
};
const int reg_nt_nexports = sizeof(reg_nt_exports) / sizeof(reg_nt_exports[0]);
