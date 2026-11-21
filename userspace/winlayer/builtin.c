/* The builtin DLLs: which one answers for a DLL name, and finding an export.
 *
 * Programs name system DLLs in many ways: "KERNEL32.dll", "kernelbase",
 * api sets ("api-ms-win-crt-stdio-l1-1-0.dll" is the UCRT), and older or
 * newer runtimes (msvcr120 is msvcrt to us).  Each builtin's exports are put
 * in a hash table the first time it is asked, since GetProcAddress is
 * called by name hundreds of times (OpenGL above all). */
#include "win.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static const wdll_t *const all_dlls[] = {
    &dll_kernel32, &dll_ntdll, &dll_msvcrt, &dll_ucrt, &dll_vcruntime, &dll_user32, &dll_gdi32, &dll_opengl32,
    &dll_winmm, &dll_advapi32, &dll_shell32, &dll_ole32, &dll_oleaut32, &dll_imm32, &dll_setupapi, &dll_version,
    &dll_ws2_32, &dll_bcrypt, &dll_cabinet, 0
};

static const struct {
    const char   *name;
    const wdll_t *dll;
} aliases[] = {
    { "kernelbase.dll", &dll_kernel32 },
    { "ucrtbase.dll", &dll_ucrt },
    { "vcruntime140_1.dll", &dll_vcruntime },
    { "msvcr80.dll", &dll_msvcrt },
    { "msvcr90.dll", &dll_msvcrt },
    { "msvcr100.dll", &dll_msvcrt },
    { "msvcr110.dll", &dll_msvcrt },
    { "msvcr120.dll", &dll_msvcrt },
    { "wsock32.dll", &dll_ws2_32 },
    { "combase.dll", &dll_ole32 },
    { "bcryptprimitives.dll", &dll_bcrypt },
};

/* api-ms-win-* names, by prefix */
static const struct {
    const char   *prefix;
    const wdll_t *dll;
} apisets[] = {
    { "api-ms-win-crt-", &dll_ucrt },
    { "api-ms-win-core-", &dll_kernel32 },
    { "api-ms-win-security-", &dll_advapi32 },
    { "api-ms-win-eventing-", &dll_advapi32 },
    { "api-ms-win-shcore-", &dll_shell32 },
    { "api-ms-win-shell-", &dll_shell32 },
};

static void lower_copy(const char *s, char *out, size_t cap) {
    size_t i = 0;
    for (; s[i] && i + 5 < cap; i++) out[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    out[i] = 0;
    if (!strchr(out, '.')) strcat(out, ".dll");
}

const wdll_t *builtin_find(const char *dll) {
    char n[96];
    const char *b = dll;
    for (const char *p = dll; *p; p++)
        if (*p == '\\' || *p == '/') b = p + 1;
    lower_copy(b, n, sizeof(n));
    for (int i = 0; all_dlls[i]; i++)
        if (!strcmp(n, all_dlls[i]->name)) return all_dlls[i];
    for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
        if (!strcmp(n, aliases[i].name)) return aliases[i].dll;
    for (unsigned i = 0; i < sizeof(apisets) / sizeof(apisets[0]); i++)
        if (!strncmp(n, apisets[i].prefix, strlen(apisets[i].prefix))) return apisets[i].dll;
    if (!strncmp(n, "api-ms-", 7) || !strncmp(n, "ext-ms-", 7)) return &dll_misc;
    return 0;
}

/* ---- lookup ---------------------------------------------------------------------------- */

typedef struct {
    const wexport_t *ex;
    int              cap;          /* a power of two */
    int             *slot;         /* export index + 1, 0: empty */
} xtable_t;

static xtable_t tables[64];
static pthread_mutex_t tlock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static xtable_t *table_of(const wexport_t *ex, int count) {
    xtable_t *t = 0;
    pthread_mutex_lock(&tlock);
    for (int i = 0; i < 64; i++) {
        if (tables[i].ex == ex) {
            t = &tables[i];
            break;
        }
        if (!tables[i].ex) {
            int cap = 64;
            t = &tables[i];
            while (cap < count * 2) cap <<= 1;
            t->slot = calloc((size_t)cap, sizeof(int));
            t->cap = cap;
            for (int k = 0; k < count; k++) {
                uint32_t h;
                if (!ex[k].name) continue;
                h = hash(ex[k].name) & (uint32_t)(cap - 1);
                while (t->slot[h]) h = (h + 1) & (uint32_t)(cap - 1);
                t->slot[h] = k + 1;
            }
            __atomic_store_n(&t->ex, ex, __ATOMIC_RELEASE);
            break;
        }
    }
    pthread_mutex_unlock(&tlock);
    return t;
}

static void *lookup(const wexport_t *ex, int count, const char *name, int ordinal) {
    if (!ex || count <= 0) return 0;
    if (!name) {
        for (int k = 0; k < count; k++)
            if (ex[k].ordinal == ordinal && ordinal) return ex[k].fn;
        return 0;
    }
    {
        xtable_t *t = table_of(ex, count);
        uint32_t h;
        if (!t) {
            for (int k = 0; k < count; k++)
                if (ex[k].name && !strcmp(ex[k].name, name)) return ex[k].fn;
            return 0;
        }
        h = hash(name) & (uint32_t)(t->cap - 1);
        while (t->slot[h]) {
            const wexport_t *e = &ex[t->slot[h] - 1];
            if (!strcmp(e->name, name)) return e->fn;
            h = (h + 1) & (uint32_t)(t->cap - 1);
        }
    }
    return 0;
}

void *builtin_proc(const wdll_t *d, const char *name, int ordinal) {
    void *p;
    if (!d) return 0;
    p = lookup(d->exports, d->count, name, ordinal);
    if (!p && d->more) p = lookup(d->more, *d->nmore, name, ordinal);
    return p;
}

/* DLLs a program may name but that ICDA has nothing of: loading one gives a
 * module whose functions are stubs */
const wdll_t dll_misc = { "misc.dll", 0, 0 };
