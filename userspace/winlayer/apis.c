/* What each Windows API function returns, from MinGW-w64's headers
 * (apis_gen.h, made by gen_apis.py): a function the layer does not have
 * fails the way its callers check for (FALSE, NULL, E_NOTIMPL, an NTSTATUS
 * or an error code), and functions imported by number have their names. */
#include "win.h"
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "apis_gen.h"

static const char *entries[API_COUNT];
static pthread_once_t once = PTHREAD_ONCE_INIT;

/* each DLL's exports: its name and its functions' names, sorted */
typedef struct {
    const char  *dll;
    const char **names;
    int          n;
} dll_exports_t;
static dll_exports_t dlls[160];
static int ndlls;
static const char *export_names[API_EXPORTS];

static void index_table(void) {
    const char *p = api_table;
    int used = 0;
    for (int i = 0; i < API_COUNT; i++) {
        entries[i] = p;
        p += strlen(p) + 1;            /* the name */
        p += 2;                        /* its kind and NUL */
    }
    for (p = api_exports; *p && ndlls < (int)(sizeof(dlls) / sizeof(dlls[0]));) {
        dll_exports_t *d = &dlls[ndlls++];
        d->dll = p;
        p += strlen(p) + 1;
        d->names = &export_names[used];
        while (*p && used < API_EXPORTS) {
            export_names[used++] = p;
            d->n++;
            p += strlen(p) + 1;
        }
        p++;                           /* the empty name that ends it */
    }
}

static void lower_dll(const char *dll, char *low, size_t cap) {
    size_t n = 0;
    const char *b = dll;
    for (const char *q = dll; q && *q; q++)
        if (*q == '\\' || *q == '/') b = q + 1;
    for (; b && b[n] && n + 5 < cap; n++) low[n] = (char)(b[n] >= 'A' && b[n] <= 'Z' ? b[n] + 32 : b[n]);
    low[n] = 0;
    if (!strchr(low, '.')) strcat(low, ".dll");
}

/* the DLL (Windows') exports a function of that name */
int api_exported(const char *dll, const char *name) {
    char low[64];
    if (!dll || !name) return 0;
    pthread_once(&once, index_table);
    lower_dll(dll, low, sizeof(low));
    for (int i = 0; i < ndlls; i++) {
        int lo = 0, hi = dlls[i].n - 1;
        if (strcmp(dlls[i].dll, low)) continue;
        while (lo <= hi) {
            int mid = (lo + hi) / 2, c = strcmp(name, dlls[i].names[mid]);
            if (!c) return 1;
            if (c < 0) hi = mid - 1;
            else lo = mid + 1;
        }
        return 0;
    }
    return 0;
}

/* the function's kind (see gen_apis.py), 0 if it is not known */
int api_kind(const char *name) {
    int lo = 0, hi = API_COUNT - 1;
    if (!name) return 0;
    pthread_once(&once, index_table);
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, entries[mid]);
        if (!c) return entries[mid][strlen(entries[mid]) + 1];
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return 0;
}

/* the name of a DLL's export number, 0 if not known */
const char *api_ordinal_name(const char *dll, int ordinal) {
    char low[64];
    lower_dll(dll, low, sizeof(low));
    for (size_t i = 0; i < sizeof(api_ordinals) / sizeof(api_ordinals[0]); i++)
        if (api_ordinals[i].ord == ordinal && !strcmp(api_ordinals[i].dll, low)) return api_ordinals[i].name;
    return 0;
}

/* what a missing function returns, by its kind; *known: the kind was found */
uint64_t api_failure(const char *name, int *known) {
    int k = api_kind(name);
    *known = k != 0 && k != 'i';
    switch (k) {
    case 'h': return 0x80004001u;                  /* E_NOTIMPL */
    case 'n': return 0xC0000002u;                  /* STATUS_NOT_IMPLEMENTED */
    case 'e': return 120;                          /* ERROR_CALL_NOT_IMPLEMENTED */
    case 'm': return 8;                            /* MMSYSERR_NOTSUPPORTED */
    case 'c': return 0x34;                         /* CR_CALL_NOT_IMPLEMENTED */
    case 's': return 0x80090302u;                  /* SEC_E_UNSUPPORTED_FUNCTION */
    case 'H': return (uint64_t)-1;                 /* INVALID_HANDLE_VALUE */
    default: return 0;                             /* FALSE, NULL, nothing */
    }
}
