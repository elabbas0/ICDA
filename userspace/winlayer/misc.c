/* The smaller system DLLs: advapi32 (registry, security, random numbers),
 * shell32 (folders, the command line), ole32/oleaut32 (COM, which has no
 * classes here), imm32 (input methods), setupapi (devices), version and
 * bcrypt.
 *
 * The registry is empty: programs ask it for optional settings and use
 * their defaults.  COM has no classes, so programs fall back to the older
 * APIs (SDL, for one, goes from WASAPI to winmm for sound). */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/random.h>

HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T n);
int path_find(const char *wpath, char *out, size_t cap);

static void fill_random(void *p, size_t n) {
    uint8_t *b = p;
    while (n) {
        ssize_t r = getrandom(b, n, 0);
        if (r <= 0) {
            /* no getrandom: a weak fill is still better than zeros */
            uint64_t x = mono_ns() ^ (uintptr_t)p;
            for (size_t i = 0; i < n; i++) {
                x ^= x << 13, x ^= x >> 7, x ^= x << 17;
                b[i] = (uint8_t)x;
            }
            return;
        }
        b += r;
        n -= (size_t)r;
    }
}

/* ---- advapi32 ------------------------------------------------------------------------------- */

#define HKEY_FAKE ((HKEY)(uintptr_t)0x7E570000)

static LONG WINAPI RegOpenKeyExW(HKEY k, const WCHAR *sub, DWORD opt, DWORD sam, HKEY *out) {
    (void)k, (void)sub, (void)opt, (void)sam;
    if (out) *out = 0;
    return ERROR_FILE_NOT_FOUND;
}
static LONG WINAPI RegOpenKeyExA(HKEY k, const char *sub, DWORD opt, DWORD sam, HKEY *out) {
    (void)k, (void)sub, (void)opt, (void)sam;
    if (out) *out = 0;
    return ERROR_FILE_NOT_FOUND;
}
static LONG WINAPI RegOpenKeyW(HKEY k, const WCHAR *sub, HKEY *out) { return RegOpenKeyExW(k, sub, 0, 0, out); }
static LONG WINAPI RegOpenKeyA(HKEY k, const char *sub, HKEY *out) { return RegOpenKeyExA(k, sub, 0, 0, out); }
static LONG WINAPI RegCreateKeyExW(HKEY k, const WCHAR *sub, DWORD r, WCHAR *cls, DWORD opt, DWORD sam, void *sa, HKEY *out,
                                   DWORD *disp) {
    (void)k, (void)sub, (void)r, (void)cls, (void)opt, (void)sam, (void)sa;
    if (out) *out = HKEY_FAKE;
    if (disp) *disp = 1;
    return 0;
}
static LONG WINAPI RegCreateKeyExA(HKEY k, const char *sub, DWORD r, char *cls, DWORD opt, DWORD sam, void *sa, HKEY *out,
                                   DWORD *disp) {
    (void)k, (void)sub, (void)r, (void)cls, (void)opt, (void)sam, (void)sa;
    if (out) *out = HKEY_FAKE;
    if (disp) *disp = 1;
    return 0;
}
static LONG WINAPI RegCloseKey(HKEY k) {
    (void)k;
    return 0;
}
static LONG WINAPI RegQueryValueExW(HKEY k, const WCHAR *n, DWORD *r, DWORD *t, BYTE *d, DWORD *sz) {
    (void)k, (void)n, (void)r, (void)t, (void)d, (void)sz;
    return ERROR_FILE_NOT_FOUND;
}
static LONG WINAPI RegQueryValueExA(HKEY k, const char *n, DWORD *r, DWORD *t, BYTE *d, DWORD *sz) {
    (void)k, (void)n, (void)r, (void)t, (void)d, (void)sz;
    return ERROR_FILE_NOT_FOUND;
}
static LONG WINAPI RegGetValueW(HKEY k, const WCHAR *s, const WCHAR *v, DWORD f, DWORD *t, void *d, DWORD *sz) {
    (void)k, (void)s, (void)v, (void)f, (void)t, (void)d, (void)sz;
    return ERROR_FILE_NOT_FOUND;
}
static LONG WINAPI RegGetValueA(HKEY k, const char *s, const char *v, DWORD f, DWORD *t, void *d, DWORD *sz) {
    (void)k, (void)s, (void)v, (void)f, (void)t, (void)d, (void)sz;
    return ERROR_FILE_NOT_FOUND;
}
static LONG WINAPI RegSetValueExW(HKEY k, const WCHAR *n, DWORD r, DWORD t, const BYTE *d, DWORD sz) {
    (void)k, (void)n, (void)r, (void)t, (void)d, (void)sz;
    return 0;
}
static LONG WINAPI RegSetValueExA(HKEY k, const char *n, DWORD r, DWORD t, const BYTE *d, DWORD sz) {
    (void)k, (void)n, (void)r, (void)t, (void)d, (void)sz;
    return 0;
}
static LONG WINAPI RegDeleteValueW(HKEY k, const WCHAR *n) {
    (void)k, (void)n;
    return ERROR_FILE_NOT_FOUND;
}
static LONG WINAPI RegDeleteKeyW(HKEY k, const WCHAR *n) {
    (void)k, (void)n;
    return ERROR_FILE_NOT_FOUND;
}
static LONG WINAPI RegEnumKeyExW(HKEY k, DWORD i, WCHAR *n, DWORD *nn, DWORD *r, WCHAR *c, DWORD *cn, FILETIME *ft) {
    (void)k, (void)i, (void)n, (void)nn, (void)r, (void)c, (void)cn, (void)ft;
    return ERROR_NO_MORE_ITEMS;
}
static LONG WINAPI RegEnumValueW(HKEY k, DWORD i, WCHAR *n, DWORD *nn, DWORD *r, DWORD *t, BYTE *d, DWORD *dn) {
    (void)k, (void)i, (void)n, (void)nn, (void)r, (void)t, (void)d, (void)dn;
    return ERROR_NO_MORE_ITEMS;
}
static LONG WINAPI RegQueryInfoKeyW(HKEY k, WCHAR *c, DWORD *cn, DWORD *r, DWORD *nsub, DWORD *maxsub, DWORD *maxcls,
                                    DWORD *nval, DWORD *maxvn, DWORD *maxvl, DWORD *sd, FILETIME *ft) {
    (void)k, (void)c, (void)cn, (void)r, (void)sd, (void)ft;
    if (nsub) *nsub = 0;
    if (maxsub) *maxsub = 0;
    if (maxcls) *maxcls = 0;
    if (nval) *nval = 0;
    if (maxvn) *maxvn = 0;
    if (maxvl) *maxvl = 0;
    return 0;
}
static LONG WINAPI RegNotifyChangeKeyValue(HKEY k, BOOL sub, DWORD f, HANDLE ev, BOOL async) {
    (void)k, (void)sub, (void)f, (void)ev, (void)async;
    return 0;
}
static LONG WINAPI RegOpenCurrentUser(DWORD sam, HKEY *out) {
    (void)sam;
    *out = (HKEY)(uintptr_t)0x80000001;
    return 0;
}

static BOOL WINAPI GetUserNameW(WCHAR *buf, DWORD *n) {
    const char *u = getenv("USER");
    int len;
    if (!u || !*u) u = "user";
    len = utf8_to_w(u, -1, 0, 0);
    if ((DWORD)len > *n) {
        *n = (DWORD)len;
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    utf8_to_w(u, -1, buf, (int)*n);
    *n = (DWORD)len;
    return TRUE;
}
static BOOL WINAPI GetUserNameA(char *buf, DWORD *n) {
    const char *u = getenv("USER");
    size_t len;
    if (!u || !*u) u = "user";
    len = strlen(u) + 1;
    if (len > *n) {
        *n = (DWORD)len;
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(buf, u, len);
    *n = (DWORD)len;
    return TRUE;
}

static BOOL WINAPI CryptAcquireContextW(void **h, const WCHAR *c, const WCHAR *p, DWORD t, DWORD f) {
    (void)c, (void)p, (void)t, (void)f;
    *h = (void *)(uintptr_t)0xC0FFEE;
    return TRUE;
}
static BOOL WINAPI CryptAcquireContextA(void **h, const char *c, const char *p, DWORD t, DWORD f) {
    (void)c, (void)p, (void)t, (void)f;
    *h = (void *)(uintptr_t)0xC0FFEE;
    return TRUE;
}
static BOOL WINAPI CryptReleaseContext(void *h, DWORD f) {
    (void)h, (void)f;
    return TRUE;
}
static BOOL WINAPI CryptGenRandom(void *h, DWORD n, BYTE *b) {
    (void)h;
    fill_random(b, n);
    return TRUE;
}
static BOOLEAN WINAPI SystemFunction036(void *b, ULONG n) {
    fill_random(b, n);
    return TRUE;
}
static BOOL WINAPI CryptFail(void) {
    wset_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

static BOOL WINAPI OpenProcessToken(HANDLE p, DWORD a, HANDLE *t) {
    (void)p, (void)a;
    *t = (HANDLE)(uintptr_t)0x70C0;
    return TRUE;
}
static BOOL WINAPI OpenThreadToken(HANDLE th, DWORD a, BOOL self, HANDLE *t) {
    (void)th, (void)a, (void)self;
    *t = 0;
    wset_last_error(1008);   /* ERROR_NO_TOKEN */
    return FALSE;
}
static BOOL WINAPI GetTokenInformation(HANDLE t, int cls, void *info, DWORD n, DWORD *ret) {
    (void)t;
    if (cls == 20 && n >= 4) {     /* TokenElevation */
        *(DWORD *)info = 0;
        if (ret) *ret = 4;
        return TRUE;
    }
    if (ret) *ret = 0;
    wset_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
static BOOL WINAPI AllocateAndInitializeSid(void *auth, BYTE n, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g,
                                            DWORD h, void **sid) {
    (void)auth, (void)n, (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h;
    *sid = calloc(1, 64);
    return TRUE;
}
static void *WINAPI FreeSid(void *sid) {
    free(sid);
    return 0;
}
static BOOL WINAPI CheckTokenMembership(HANDLE t, void *sid, BOOL *is) {
    (void)t, (void)sid;
    *is = FALSE;
    return TRUE;
}
static BOOL WINAPI SecTrue(void) { return TRUE; }
static ULONG WINAPI EventRegister(const GUID *g, void *cb, void *ctx, uint64_t *h) {
    (void)g, (void)cb, (void)ctx;
    *h = 1;
    return 0;
}
static ULONG WINAPI EventZero(void) { return 0; }

static const wexport_t advapi[] = {
    WEXP(RegOpenKeyExW), WEXP(RegOpenKeyExA), WEXP(RegOpenKeyW), WEXP(RegOpenKeyA), WEXP(RegCreateKeyExW),
    WEXP(RegCreateKeyExA), WEXP(RegCloseKey), WEXP(RegQueryValueExW), WEXP(RegQueryValueExA), WEXP(RegGetValueW),
    WEXP(RegGetValueA), WEXP(RegSetValueExW), WEXP(RegSetValueExA), WEXP(RegDeleteValueW), WEXP(RegDeleteKeyW),
    WEXP(RegEnumKeyExW), WEXP(RegEnumValueW), WEXP(RegQueryInfoKeyW), WEXP(RegNotifyChangeKeyValue),
    WEXP(RegOpenCurrentUser), WEXP(GetUserNameW), WEXP(GetUserNameA), WEXP(CryptAcquireContextW),
    WEXP(CryptAcquireContextA), WEXP(CryptReleaseContext), WEXP(CryptGenRandom), WEXP(SystemFunction036),
    WEXPN("CryptCreateHash", CryptFail), WEXPN("CryptHashData", CryptFail), WEXPN("CryptGetHashParam", CryptFail),
    WEXPN("CryptDestroyHash", SecTrue), WEXPN("CryptImportKey", CryptFail), WEXPN("CryptDestroyKey", SecTrue),
    WEXPN("CryptEncrypt", CryptFail), WEXPN("CryptDecrypt", CryptFail), WEXP(OpenProcessToken), WEXP(OpenThreadToken),
    WEXP(GetTokenInformation), WEXP(AllocateAndInitializeSid), WEXP(FreeSid), WEXP(CheckTokenMembership),
    WEXPN("InitializeSecurityDescriptor", SecTrue), WEXPN("SetSecurityDescriptorDacl", SecTrue), WEXP(EventRegister),
    WEXPN("EventUnregister", EventZero), WEXPN("EventWriteTransfer", EventZero), WEXPN("EventWrite", EventZero),
    WEXPN("EventSetInformation", EventZero), WEXPN("EventEnabled", EventZero),
};
const wdll_t dll_advapi32 = { "advapi32.dll", advapi, sizeof(advapi) / sizeof(advapi[0]) };

/* ---- bcrypt ---------------------------------------------------------------------------------- */

static LONG WINAPI BCryptGenRandom(void *alg, BYTE *b, ULONG n, ULONG f) {
    (void)alg, (void)f;
    fill_random(b, n);
    return 0;
}
static LONG WINAPI BCryptNotSupported(void) { return (LONG)0xC00000BB; }   /* STATUS_NOT_SUPPORTED */

static const wexport_t bcrypt[] = {
    WEXP(BCryptGenRandom), WEXPN("BCryptOpenAlgorithmProvider", BCryptNotSupported),
    WEXPN("BCryptCloseAlgorithmProvider", EventZero),
};
const wdll_t dll_bcrypt = { "bcrypt.dll", bcrypt, sizeof(bcrypt) / sizeof(bcrypt[0]) };

/* ---- shell32 ---------------------------------------------------------------------------------- */

/* Windows's rules for splitting a command line */
static WCHAR **WINAPI CommandLineToArgvW(const WCHAR *cmd, int *argc) {
    size_t n = w_len(cmd), cap = n + 2;
    WCHAR *strs = malloc(cap * sizeof(WCHAR)), *o = strs;
    WCHAR *starts[512];
    int count = 0;
    const WCHAR *p = cmd;
    WCHAR **out;
    char *block;
    /* the program's name: up to the next space, or quoted, with no escapes */
    starts[count++] = o;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') *o++ = *p++;
        if (*p) p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') *o++ = *p++;
    }
    *o++ = 0;
    for (;;) {
        int inq = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || count >= 511) break;
        starts[count++] = o;
        for (; *p; p++) {
            size_t bs = 0;
            while (*p == '\\') bs++, p++;
            if (*p == '"') {
                for (size_t i = 0; i < bs / 2; i++) *o++ = '\\';
                if (bs % 2) {
                    *o++ = '"';
                    continue;
                }
                if (inq && p[1] == '"') {
                    *o++ = '"';
                    p++;
                    continue;
                }
                inq = !inq;
                continue;
            }
            for (size_t i = 0; i < bs; i++) *o++ = '\\';
            if (!*p) break;
            if (!inq && (*p == ' ' || *p == '\t')) break;
            *o++ = *p;
        }
        *o++ = 0;
    }
    /* one block: the pointers, then the strings, so LocalFree frees it all */
    block = LocalAlloc(0, (size_t)(count + 1) * sizeof(WCHAR *) + (size_t)(o - strs) * sizeof(WCHAR));
    out = (WCHAR **)block;
    memcpy(block + (size_t)(count + 1) * sizeof(WCHAR *), strs, (size_t)(o - strs) * sizeof(WCHAR));
    for (int i = 0; i < count; i++)
        out[i] = (WCHAR *)(block + (size_t)(count + 1) * sizeof(WCHAR *)) + (starts[i] - strs);
    out[count] = 0;
    free(strs);
    *argc = count;
    return out;
}

static const char *user_name(void) {
    const char *u = getenv("USER");
    return u && *u ? u : "user";
}

/* the folder a CSIDL names, as a Windows path; made if asked */
static int folder_path(int csidl, char *out, size_t cap, int create) {
    const char *rel;
    char tmp[512], up[1024];
    switch (csidl & 0xFF) {
    case 0x1A: rel = "AppData\\Roaming"; break;           /* CSIDL_APPDATA */
    case 0x1C: rel = "AppData\\Local"; break;             /* CSIDL_LOCAL_APPDATA */
    case 0x05: rel = "Documents"; break;                  /* CSIDL_PERSONAL */
    case 0x10: case 0x00: rel = "Desktop"; break;         /* CSIDL_DESKTOPDIRECTORY */
    case 0x27: rel = "Pictures"; break;
    case 0x0D: rel = "Music"; break;
    case 0x0E: rel = "Videos"; break;
    case 0x28: rel = ""; break;                           /* CSIDL_PROFILE */
    case 0x23: snprintf(out, cap, "C:\\ProgramData"); goto made;
    case 0x26: case 0x2A: snprintf(out, cap, "C:\\Program Files"); goto made;
    case 0x24: snprintf(out, cap, "C:\\Windows"); goto made;
    case 0x25: case 0x29: snprintf(out, cap, "C:\\Windows\\System32"); goto made;
    case 0x2B: snprintf(out, cap, "C:\\Program Files\\Common Files"); goto made;
    case 0x15: rel = "AppData\\Roaming\\Microsoft\\Windows\\Templates"; break;
    case 0x20: rel = "AppData\\Local\\Microsoft\\Windows\\INetCache"; break;
    default: return -1;
    }
    snprintf(tmp, sizeof(tmp), "C:\\Users\\%s%s%s", user_name(), *rel ? "\\" : "", rel);
    snprintf(out, cap, "%s", tmp);
made:
    if (create || 1) {
        /* made always: programs often write there without asking for that */
        if (path_find(out, up, sizeof(up)) == 0) {
            char *p = up;
            for (p = up + 1; *p; p++)
                if (*p == '/') {
                    *p = 0;
                    mkdir(up, 0755);
                    *p = '/';
                }
            mkdir(up, 0755);
        }
    }
    return 0;
}

static HRESULT WINAPI SHGetFolderPathW(HWND w, int csidl, HANDLE t, DWORD f, WCHAR *path) {
    char p[512];
    (void)w, (void)t, (void)f;
    if (folder_path(csidl, p, sizeof(p), (csidl & 0x8000) != 0) != 0) return (HRESULT)0x80070057;  /* E_INVALIDARG */
    utf8_to_w(p, -1, path, MAX_PATH);
    return 0;
}

static HRESULT WINAPI SHGetFolderPathA(HWND w, int csidl, HANDLE t, DWORD f, char *path) {
    char p[512];
    (void)w, (void)t, (void)f;
    if (folder_path(csidl, p, sizeof(p), (csidl & 0x8000) != 0) != 0) return (HRESULT)0x80070057;
    snprintf(path, MAX_PATH, "%s", p);
    return 0;
}

static BOOL WINAPI SHGetSpecialFolderPathW(HWND w, WCHAR *path, int csidl, BOOL create) {
    return SHGetFolderPathW(w, csidl | (create ? 0x8000 : 0), 0, 0, path) == 0;
}

static BOOL WINAPI SHGetSpecialFolderPathA(HWND w, char *path, int csidl, BOOL create) {
    return SHGetFolderPathA(w, csidl | (create ? 0x8000 : 0), 0, 0, path) == 0;
}

static const struct {
    GUID g;
    int  csidl;
} known[] = {
    { { 0x3EB685DB, 0x65F9, 0x4CF6, { 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D } }, 0x1A },   /* RoamingAppData */
    { { 0xF1B32785, 0x6FBA, 0x4FCF, { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 } }, 0x1C },   /* LocalAppData */
    { { 0xFDD39AD0, 0x238F, 0x46AF, { 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7 } }, 0x05 },   /* Documents */
    { { 0x5E6C858F, 0x0E22, 0x4760, { 0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73 } }, 0x28 },   /* Profile */
    { { 0xB4BFCC3A, 0xDB2C, 0x424C, { 0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41 } }, 0x10 },   /* Desktop */
    { { 0x62AB5D82, 0xFDC1, 0x4DC3, { 0xA9, 0xDD, 0x07, 0x0D, 0x1D, 0x49, 0x5D, 0x97 } }, 0x23 },   /* ProgramData */
    { { 0x33E28130, 0x4E1E, 0x4676, { 0x83, 0x5A, 0x98, 0x39, 0x5C, 0x3B, 0xC3, 0xBB } }, 0x27 },   /* Pictures */
    { { 0x905E63B6, 0xC1BF, 0x494E, { 0xB2, 0x9C, 0x65, 0xB7, 0x32, 0xD3, 0xD2, 0x1A } }, 0x26 },   /* ProgramFiles */
};

static HRESULT WINAPI SHGetKnownFolderPath(const GUID *id, DWORD flags, HANDLE t, WCHAR **out) {
    char p[512];
    (void)t;
    *out = 0;
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (!memcmp(id, &known[i].g, sizeof(GUID))) {
            folder_path(known[i].csidl, p, sizeof(p), (flags & 0x8000) != 0);
            *out = utf8_to_w_dup(p);
            return 0;
        }
    }
    /* Saved Games, LocalAppDataLow and others: under the profile */
    {
        char tmp[600];
        folder_path(0x28, p, sizeof(p), 1);
        if (id->Data1 == 0x4C5C32FF) snprintf(tmp, sizeof(tmp), "%s\\Saved Games", p);
        else if (id->Data1 == 0xA520A1A4) snprintf(tmp, sizeof(tmp), "%s\\AppData\\LocalLow", p);
        else return (HRESULT)0x80070002;    /* HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) */
        {
            char up[1024];
            if (path_find(tmp, up, sizeof(up)) == 0) mkdir(up, 0755);
        }
        *out = utf8_to_w_dup(tmp);
        return 0;
    }
}

static int WINAPI SHCreateDirectoryExW(HWND w, const WCHAR *path, void *sa) {
    char a[1024], up[1024];
    (void)w, (void)sa;
    w_to_utf8(path, -1, a, sizeof(a));
    if (path_find(a, up, sizeof(up)) != 0) return ERROR_PATH_NOT_FOUND;
    for (char *p = up + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(up, 0755);
            *p = '/';
        }
    return mkdir(up, 0755) == 0 ? 0 : (errno == EEXIST ? ERROR_ALREADY_EXISTS : ERROR_PATH_NOT_FOUND);
}

static HINSTANCE WINAPI ShellExecuteW(HWND w, const WCHAR *op, const WCHAR *file, const WCHAR *params, const WCHAR *dir, int show) {
    char a[1024] = "";
    (void)w, (void)op, (void)params, (void)dir, (void)show;
    if (file) w_to_utf8(file, -1, a, sizeof(a));
    wlog("ShellExecute(%s): not supported", a);
    return (HINSTANCE)(uintptr_t)2;    /* SE_ERR_FNF */
}

static HINSTANCE WINAPI ShellExecuteA(HWND w, const char *op, const char *file, const char *params, const char *dir, int show) {
    (void)w, (void)op, (void)params, (void)dir, (void)show;
    wlog("ShellExecute(%s): not supported", file ? file : "");
    return (HINSTANCE)(uintptr_t)2;
}

static BOOL WINAPI ShellExecuteExW(void *info) {
    (void)info;
    wset_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

static void WINAPI DragAcceptFiles(HWND w, BOOL a) { (void)w, (void)a; }
static UINT WINAPI DragQueryFileW(HANDLE d, UINT i, WCHAR *f, UINT n) {
    (void)d, (void)i, (void)f, (void)n;
    return 0;
}
static void WINAPI DragFinish(HANDLE d) { (void)d; }
static HRESULT WINAPI ShellNotImpl(void) { return (HRESULT)0x80004001; }   /* E_NOTIMPL */
static BOOL WINAPI ShellFalse(void) { return FALSE; }
static BOOL WINAPI ShellTrue(void) { return TRUE; }
static void *WINAPI ShellNull(void) { return 0; }

static const wexport_t shell[] = {
    WEXP(CommandLineToArgvW), WEXP(SHGetFolderPathW), WEXP(SHGetFolderPathA), WEXP(SHGetSpecialFolderPathW),
    WEXP(SHGetSpecialFolderPathA), WEXP(SHGetKnownFolderPath), WEXP(SHCreateDirectoryExW), WEXP(ShellExecuteW),
    WEXP(ShellExecuteA), WEXP(ShellExecuteExW), WEXP(DragAcceptFiles), WEXP(DragQueryFileW), WEXP(DragFinish),
    WEXPN("SHOpenFolderAndSelectItems", ShellNotImpl), WEXPN("SHParseDisplayName", ShellNotImpl),
    WEXPN("SHGetSpecialFolderLocation", ShellNotImpl), WEXPN("SHGetPathFromIDListW", ShellFalse),
    WEXPN("SHBrowseForFolderW", ShellNull), WEXPN("Shell_NotifyIconW", ShellTrue), WEXPN("IsUserAnAdmin", ShellFalse),
    WEXPN("SHGetFileInfoW", ShellNull), WEXPN("ExtractIconW", ShellNull), WEXPN("SHFileOperationW", ShellNotImpl),
    WEXPN("SHAppBarMessage", ShellNull), WEXPN("SHCreateItemFromParsingName", ShellNotImpl),
    WEXPN("SetCurrentProcessExplicitAppUserModelID", ShellNotImpl),
};
const wdll_t dll_shell32 = { "shell32.dll", shell, sizeof(shell) / sizeof(shell[0]) };

/* ---- ole32 / oleaut32 ---------------------------------------------------------------------- */

static HRESULT WINAPI CoInitializeEx(void *r, DWORD m) {
    (void)r, (void)m;
    return 0;
}
static HRESULT WINAPI CoInitialize(void *r) {
    (void)r;
    return 0;
}
static void WINAPI CoUninitialize(void) {}
static HRESULT WINAPI CoCreateInstance(const GUID *cls, void *outer, DWORD ctx, const GUID *iid, void **out) {
    (void)outer, (void)ctx, (void)iid;
    if (out) *out = 0;
    if (wtrace) wlog("CoCreateInstance({%08X-...}): no such class here", cls ? cls->Data1 : 0);
    return (HRESULT)0x80040154;     /* REGDB_E_CLASSNOTREG */
}
static void *WINAPI CoTaskMemAlloc(SIZE_T n) { return malloc(n ? n : 1); }
static void *WINAPI CoTaskMemRealloc(void *p, SIZE_T n) { return realloc(p, n ? n : 1); }
static void WINAPI CoTaskMemFree(void *p) { free(p); }

static int hexv(WCHAR c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static HRESULT WINAPI CLSIDFromString(const WCHAR *s, GUID *g) {
    /* {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} */
    uint8_t b[16];
    int bi = 0;
    if (!s || s[0] != '{' || w_len(s) != 38) return (HRESULT)0x800401F3;   /* CO_E_CLASSSTRING */
    for (int i = 1; i < 37 && bi < 16; i++) {
        int h, l;
        if (s[i] == '-') continue;
        h = hexv(s[i]), l = hexv(s[i + 1]);
        if (h < 0 || l < 0) return (HRESULT)0x800401F3;
        b[bi++] = (uint8_t)(h << 4 | l);
        i++;
    }
    g->Data1 = (DWORD)b[0] << 24 | (DWORD)b[1] << 16 | (DWORD)b[2] << 8 | b[3];
    g->Data2 = (WORD)(b[4] << 8 | b[5]);
    g->Data3 = (WORD)(b[6] << 8 | b[7]);
    memcpy(g->Data4, b + 8, 8);
    return 0;
}

static HRESULT WINAPI IIDFromString(const WCHAR *s, GUID *g) { return CLSIDFromString(s, g); }

static int WINAPI StringFromGUID2(const GUID *g, WCHAR *out, int n) {
    char a[64];
    if (n < 39) return 0;
    snprintf(a, sizeof(a), "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->Data1, g->Data2, g->Data3, g->Data4[0],
             g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    utf8_to_w(a, -1, out, n);
    return 39;
}

static HRESULT WINAPI CoCreateGuid(GUID *g) {
    fill_random(g, sizeof(*g));
    g->Data3 = (WORD)((g->Data3 & 0x0FFF) | 0x4000);
    g->Data4[0] = (BYTE)((g->Data4[0] & 0x3F) | 0x80);
    return 0;
}

static HRESULT WINAPI PropVariantClear(void *v) {
    memset(v, 0, 24);
    return 0;
}
static HRESULT WINAPI OleOk(void) { return 0; }
static HRESULT WINAPI OleFail(void) { return (HRESULT)0x80004005; }   /* E_FAIL */

static const wexport_t ole[] = {
    WEXP(CoInitializeEx), WEXP(CoInitialize), WEXP(CoUninitialize), WEXP(CoCreateInstance), WEXP(CoTaskMemAlloc),
    WEXP(CoTaskMemRealloc), WEXP(CoTaskMemFree), WEXP(CLSIDFromString), WEXP(IIDFromString), WEXP(StringFromGUID2),
    WEXP(CoCreateGuid), WEXP(PropVariantClear), WEXPN("OleInitialize", OleOk), WEXPN("OleUninitialize", OleOk),
    WEXPN("RegisterDragDrop", OleOk), WEXPN("RevokeDragDrop", OleOk), WEXPN("CoRegisterClassObject", OleFail),
    WEXPN("CoGetClassObject", CoCreateInstance), WEXPN("CoInitializeSecurity", OleOk),
    WEXPN("CoSetProxyBlanket", OleOk), WEXPN("CoWaitForMultipleHandles", OleFail),
};
const wdll_t dll_ole32 = { "ole32.dll", ole, sizeof(ole) / sizeof(ole[0]) };

static WCHAR *WINAPI SysAllocStringLen(const WCHAR *s, UINT n) {
    uint32_t *b = malloc(4 + ((size_t)n + 1) * 2);
    WCHAR *w = (WCHAR *)(b + 1);
    b[0] = n * 2;
    if (s) memcpy(w, s, (size_t)n * 2);
    else memset(w, 0, (size_t)n * 2);
    w[n] = 0;
    return w;
}
static WCHAR *WINAPI SysAllocString(const WCHAR *s) { return s ? SysAllocStringLen(s, (UINT)w_len(s)) : 0; }
static void WINAPI SysFreeString(WCHAR *s) {
    if (s) free((uint32_t *)s - 1);
}
static UINT WINAPI SysStringLen(const WCHAR *s) { return s ? ((const uint32_t *)s)[-1] / 2 : 0; }
static UINT WINAPI SysStringByteLen(const WCHAR *s) { return s ? ((const uint32_t *)s)[-1] : 0; }
static void WINAPI VariantInit(void *v) { memset(v, 0, 24); }
static HRESULT WINAPI VariantClear(void *v) {
    memset(v, 0, 24);
    return 0;
}

static const wexport_t oleaut[] = {
    WEXPO("SysAllocString", SysAllocString, 2), WEXPO("SysAllocStringLen", SysAllocStringLen, 4),
    WEXPO("SysFreeString", SysFreeString, 6), WEXPO("SysStringLen", SysStringLen, 7),
    WEXPO("SysStringByteLen", SysStringByteLen, 149), WEXPO("VariantInit", VariantInit, 8),
    WEXPO("VariantClear", VariantClear, 9),
};
const wdll_t dll_oleaut32 = { "oleaut32.dll", oleaut, sizeof(oleaut) / sizeof(oleaut[0]) };

/* ---- imm32: no input methods ------------------------------------------------------------------ */

static void *WINAPI ImmNull(void) { return 0; }
static BOOL WINAPI ImmTrue(void) { return TRUE; }
static BOOL WINAPI ImmFalse(void) { return FALSE; }
static LONG WINAPI ImmZero(void) { return 0; }

static const wexport_t imm[] = {
    WEXPN("ImmGetContext", ImmNull), WEXPN("ImmReleaseContext", ImmTrue), WEXPN("ImmAssociateContext", ImmNull),
    WEXPN("ImmAssociateContextEx", ImmTrue), WEXPN("ImmGetCompositionStringW", ImmZero),
    WEXPN("ImmSetCompositionStringW", ImmFalse), WEXPN("ImmGetCandidateListW", ImmZero),
    WEXPN("ImmSetCandidateWindow", ImmTrue), WEXPN("ImmSetCompositionWindow", ImmTrue), WEXPN("ImmNotifyIME", ImmTrue),
    WEXPN("ImmGetIMEFileNameA", ImmZero), WEXPN("ImmGetOpenStatus", ImmFalse), WEXPN("ImmSetOpenStatus", ImmTrue),
    WEXPN("ImmGetDefaultIMEWnd", ImmNull), WEXPN("ImmDisableIME", ImmTrue), WEXPN("ImmGetConversionStatus", ImmFalse),
    WEXPN("ImmSetConversionStatus", ImmTrue), WEXPN("ImmIsIME", ImmFalse), WEXPN("ImmCreateContext", ImmNull),
    WEXPN("ImmDestroyContext", ImmTrue),
};
const wdll_t dll_imm32 = { "imm32.dll", imm, sizeof(imm) / sizeof(imm[0]) };

/* ---- setupapi: no devices to list -------------------------------------------------------------- */

static HANDLE WINAPI SetupDiGetClassDevs(const GUID *g, const void *e, HWND w, DWORD f) {
    (void)g, (void)e, (void)w, (void)f;
    wset_last_error(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
}
static BOOL WINAPI SetupNoMore(void) {
    wset_last_error(ERROR_NO_MORE_ITEMS);
    return FALSE;
}
static DWORD WINAPI CmNoDevice(void) { return 0xD; }    /* CR_NO_SUCH_DEVNODE */

static const wexport_t setupapi[] = {
    WEXPN("SetupDiGetClassDevsA", SetupDiGetClassDevs), WEXPN("SetupDiGetClassDevsW", SetupDiGetClassDevs),
    WEXPN("SetupDiEnumDeviceInfo", SetupNoMore), WEXPN("SetupDiEnumDeviceInterfaces", SetupNoMore),
    WEXPN("SetupDiDestroyDeviceInfoList", ImmTrue), WEXPN("SetupDiGetDeviceInterfaceDetailA", SetupNoMore),
    WEXPN("SetupDiGetDeviceInterfaceDetailW", SetupNoMore), WEXPN("SetupDiGetDeviceRegistryPropertyA", SetupNoMore),
    WEXPN("SetupDiGetDeviceRegistryPropertyW", SetupNoMore), WEXPN("CM_Get_Device_IDA", CmNoDevice),
    WEXPN("CM_Get_Device_IDW", CmNoDevice), WEXPN("CM_Get_Parent", CmNoDevice), WEXPN("CM_Locate_DevNodeA", CmNoDevice),
    WEXPN("CM_Locate_DevNodeW", CmNoDevice),
};
const wdll_t dll_setupapi = { "setupapi.dll", setupapi, sizeof(setupapi) / sizeof(setupapi[0]) };

/* ---- version: no version resources ------------------------------------------------------------- */

static DWORD WINAPI GetFileVersionInfoSize(const void *f, DWORD *h) {
    (void)f;
    if (h) *h = 0;
    wset_last_error(1813);
    return 0;
}

static const wexport_t version[] = {
    WEXPN("GetFileVersionInfoSizeA", GetFileVersionInfoSize), WEXPN("GetFileVersionInfoSizeW", GetFileVersionInfoSize),
    WEXPN("GetFileVersionInfoA", ImmFalse), WEXPN("GetFileVersionInfoW", ImmFalse), WEXPN("VerQueryValueA", ImmFalse),
    WEXPN("VerQueryValueW", ImmFalse), WEXPN("GetFileVersionInfoSizeExW", GetFileVersionInfoSize),
    WEXPN("GetFileVersionInfoExW", ImmFalse),
};
const wdll_t dll_version = { "version.dll", version, sizeof(version) / sizeof(version[0]) };
