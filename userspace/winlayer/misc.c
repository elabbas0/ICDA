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

/* access tokens: real handles (programs close them), of one token: the user's */
static int token_obj = 0x544F4B;

static BOOL WINAPI OpenProcessToken(HANDLE p, DWORD a, HANDLE *t) {
    (void)p, (void)a;
    *t = h_new(H_OTHER, &token_obj);
    return *t != 0;
}

static BOOL WINAPI DuplicateTokenEx(HANDLE t, DWORD a, void *sa, int il, int type, HANDLE *out) {
    (void)t, (void)a, (void)sa, (void)il, (void)type;
    *out = h_new(H_OTHER, &token_obj);
    return *out != 0;
}
static BOOL WINAPI OpenThreadToken(HANDLE th, DWORD a, BOOL self, HANDLE *t) {
    (void)th, (void)a, (void)self;
    *t = 0;
    wset_last_error(1008);   /* ERROR_NO_TOKEN */
    return FALSE;
}
/* the user's SID: S-1-5-21-1001-1002-1003-1000, a local account */
static const uint8_t user_sid[] = { 1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0xE9, 3, 0, 0, 0xEA, 3, 0, 0, 0xEB, 3, 0, 0, 0xE8, 3, 0, 0 };

static BOOL WINAPI GetTokenInformation(HANDLE t, int cls, void *info, DWORD n, DWORD *ret) {
    (void)t;
    if (cls == 20 && n >= 4) {     /* TokenElevation */
        *(DWORD *)info = 0;
        if (ret) *ret = 4;
        return TRUE;
    }
    if (cls == 1) {                /* TokenUser: SID_AND_ATTRIBUTES, then the SID */
        DWORD need = 16 + sizeof(user_sid);
        if (ret) *ret = need;
        if (n < need || !info) {
            wset_last_error(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        *(void **)info = (uint8_t *)info + 16;
        *(DWORD *)((uint8_t *)info + 8) = 0;
        memcpy((uint8_t *)info + 16, user_sid, sizeof(user_sid));
        return TRUE;
    }
    if (cls == 18 && n >= 4) {     /* TokenElevationType: default */
        *(DWORD *)info = 1;
        if (ret) *ret = 4;
        return TRUE;
    }
    /* TokenIntegrityLevel (TOKEN_MANDATORY_LABEL, then the label's SID: medium),
     * TokenOwner / TokenPrimaryGroup (a SID pointer, then the user's SID) */
    if (cls == 25 || cls == 4 || cls == 5) {
        static const uint8_t medium[] = { 1, 1, 0, 0, 0, 0, 0, 16, 0x00, 0x20, 0, 0 };
        const uint8_t *sid = cls == 25 ? medium : user_sid;
        DWORD sl = cls == 25 ? sizeof(medium) : sizeof(user_sid), need = 16 + sl;
        if (ret) *ret = need;
        if (n < need || !info) {
            wset_last_error(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        memset(info, 0, 16);
        *(void **)info = (uint8_t *)info + 16;
        if (cls == 25) *(DWORD *)((uint8_t *)info + 8) = 0x20;     /* SE_GROUP_INTEGRITY */
        memcpy((uint8_t *)info + 16, sid, sl);
        return TRUE;
    }
    /* DWORD answers: TokenSessionId (12) 1, TokenType (8) primary, TokenImpersonationLevel (9),
     * TokenIsAppContainer (29), TokenUIAccess (26), TokenVirtualizationAllowed/Enabled (23, 24),
     * TokenHasRestrictions (21), TokenIsRestricted (40), TokenMandatoryPolicy (27) */
    if ((cls == 12 || cls == 8 || cls == 9 || cls == 29 || cls == 26 || cls == 23 || cls == 24 || cls == 21 || cls == 40 ||
         cls == 27) && n >= 4) {
        *(DWORD *)info = cls == 12 ? 1 : cls == 8 ? 1 : cls == 9 ? 2 : cls == 27 ? 3 : 0;
        if (ret) *ret = 4;
        return TRUE;
    }
    /* groups and privileges: none (TokenGroups 2, TokenPrivileges 3, TokenRestrictedSids 11,
     * TokenLogonSid 28, TokenCapabilities 30); TokenAppContainerSid (31): a NULL SID;
     * TokenDefaultDacl (6): no DACL */
    if ((cls == 2 || cls == 3 || cls == 11 || cls == 28 || cls == 30 || cls == 31 || cls == 6) && n >= 8) {
        memset(info, 0, 8);
        if (ret) *ret = 8;
        return TRUE;
    }
    if (cls == 2 || cls == 3 || cls == 11 || cls == 28 || cls == 30 || cls == 31 || cls == 6 || cls == 10) {
        if (ret) *ret = cls == 10 ? 56 : 8;
        if (cls != 10 || n < 56 || !info) {
            wset_last_error(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
    }
    if (cls == 10 && n >= 56) {   /* TokenStatistics */
        memset(info, 0, 56);
        ((DWORD *)info)[0] = 0x1234;   /* TokenId */
        ((DWORD *)info)[2] = 0x3E7;    /* AuthenticationId */
        ((DWORD *)info)[8] = 1;        /* TokenType: primary */
        if (ret) *ret = 56;
        return TRUE;
    }
    if (ret) *ret = 0;
    wset_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
/* a SID: revision 1, the count, the 6-byte authority, the sub-authorities */
static BOOL WINAPI AllocateAndInitializeSid(const BYTE *auth, BYTE n, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f,
                                            DWORD g, DWORD h, void **sid) {
    DWORD sub[8] = { a, b, c, d, e, f, g, h };
    uint8_t *p;
    if (!auth || !sid || n > 8) {
        wset_last_error(ERROR_INVALID_PARAMETER);    /* Windows says so for more than 8 */
        return FALSE;
    }
    p = calloc(1, 8 + 4 * (size_t)n);
    if (!p) {
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    p[0] = 1, p[1] = n;
    memcpy(p + 2, auth, 6);
    memcpy(p + 8, sub, 4 * (size_t)n);
    *sid = p;
    return TRUE;
}
static void *WINAPI FreeSid(void *sid) {
    free(sid);
    return 0;
}
/* the one user is in the groups every signed-in user is in: Everyone (S-1-1-0),
 * Interactive (S-1-5-4), Authenticated Users (S-1-5-11) and Users
 * (S-1-5-32-545); not Administrators */
static BOOL WINAPI CheckTokenMembership(HANDLE t, const uint8_t *sid, BOOL *is) {
    static const uint8_t groups[][16] = {
        { 1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 },
        { 1, 1, 0, 0, 0, 0, 0, 5, 4, 0, 0, 0 },
        { 1, 1, 0, 0, 0, 0, 0, 5, 11, 0, 0, 0 },
        { 1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x21, 2, 0, 0 },
    };
    (void)t;
    if (!sid || !is) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *is = FALSE;
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        size_t len = 8 + 4 * (size_t)groups[i][1];
        if (sid[1] == groups[i][1] && !memcmp(sid, groups[i], len)) *is = TRUE;
    }
    return TRUE;
}
static BOOL WINAPI SecTrue(void) { return TRUE; }

static BOOL WINAPI ConvertSidToStringSidW(const uint8_t *sid, WCHAR **out) {
    char s[160];
    int n, o;
    uint64_t auth = 0;
    if (!sid || !out) return FALSE;
    for (int i = 2; i < 8; i++) auth = auth << 8 | sid[i];
    o = snprintf(s, sizeof(s), "S-%u-%llu", sid[0], (unsigned long long)auth);
    n = sid[1];
    for (int i = 0; i < n && o < (int)sizeof(s) - 12; i++) o += snprintf(s + o, sizeof(s) - (size_t)o, "-%u", *(const uint32_t *)(sid + 8 + 4 * i));
    *out = LocalAlloc(0, ((size_t)o + 1) * 2);
    utf8_to_w(s, -1, *out, o + 1);
    return TRUE;
}

static BOOL WINAPI ConvertSidToStringSidA(const uint8_t *sid, char **out) {
    WCHAR *w;
    if (!ConvertSidToStringSidW(sid, &w)) return FALSE;
    *out = LocalAlloc(0, w_len(w) * 3 + 1);
    w_to_utf8(w, -1, *out, (int)w_len(w) * 3 + 1);
    free(w);
    return TRUE;
}

/* a security descriptor that says nothing: everyone may */
static BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorW(const WCHAR *s, DWORD rev, void **sd, ULONG *n) {
    (void)s, (void)rev;
    *sd = LocalAlloc(0x40, 20);
    ((uint8_t *)*sd)[0] = 1;                  /* revision */
    *(uint16_t *)((uint8_t *)*sd + 2) = 0x8004; /* self-relative, DACL present (NULL: everyone) */
    if (n) *n = 20;
    return TRUE;
}

static DWORD WINAPI SetNamedSecurityInfoW(const WCHAR *o, int t, DWORD i, void *a, void *b, void *c, void *d) {
    (void)o, (void)t, (void)i, (void)a, (void)b, (void)c, (void)d;
    return 0;
}

static DWORD WINAPI GetNamedSecurityInfoW(const WCHAR *o, int t, DWORD i, void **a, void **b, void **c, void **d, void **sd) {
    (void)o, (void)t, (void)i;
    if (a) *a = 0;
    if (b) *b = 0;
    if (c) *c = 0;
    if (d) *d = 0;
    if (sd) *sd = LocalAlloc(0x40, 20);
    return 0;
}

static BOOL WINAPI ConvertStringSecurityDescriptorToSecurityDescriptorA(const char *s, DWORD rev, void **sd, ULONG *n) {
    WCHAR *w = utf8_to_w_dup(s);
    BOOL r = ConvertStringSecurityDescriptorToSecurityDescriptorW(w, rev, sd, n);
    free(w);
    return r;
}

static DWORD WINAPI SetNamedSecurityInfoA(char *o, int t, DWORD i, void *a, void *b, void *c, void *d) {
    WCHAR *w = utf8_to_w_dup(o);
    DWORD r = SetNamedSecurityInfoW(w, t, i, a, b, c, d);
    free(w);
    return r;
}

static DWORD WINAPI GetNamedSecurityInfoA(const char *o, int t, DWORD i, void **a, void **b, void **c, void **d, void **sd) {
    WCHAR *w = utf8_to_w_dup(o);
    DWORD r = GetNamedSecurityInfoW(w, t, i, a, b, c, d, sd);
    free(w);
    return r;
}

static BOOL WINAPI IsValidSid(const void *sid) { return sid != 0; }
static DWORD WINAPI GetLengthSid(const uint8_t *sid) { return sid ? 8u + 4u * sid[1] : 0; }
static BOOL WINAPI EqualSid(const uint8_t *a, const uint8_t *b) {
    return a && b && GetLengthSid(a) == GetLengthSid(b) && !memcmp(a, b, GetLengthSid(a));
}
static BOOL WINAPI CopySid(DWORD n, void *dst, const uint8_t *src) {
    if (n < GetLengthSid(src)) return FALSE;
    memcpy(dst, src, GetLengthSid(src));
    return TRUE;
}
static ULONG WINAPI EventRegister(const GUID *g, void *cb, void *ctx, uint64_t *h) {
    (void)g, (void)cb, (void)ctx;
    *h = 1;
    return 0;
}
static ULONG WINAPI EventZero(void) { return 0; }

static const wexport_t advapi[] = {
    WEXP(GetUserNameW), WEXP(GetUserNameA), WEXP(CryptAcquireContextW),
    WEXP(CryptAcquireContextA), WEXP(CryptReleaseContext), WEXP(CryptGenRandom), WEXP(SystemFunction036),
    WEXPN("CryptCreateHash", CryptFail), WEXPN("CryptHashData", CryptFail), WEXPN("CryptGetHashParam", CryptFail),
    WEXPN("CryptDestroyHash", SecTrue), WEXPN("CryptImportKey", CryptFail), WEXPN("CryptDestroyKey", SecTrue),
    WEXPN("CryptEncrypt", CryptFail), WEXPN("CryptDecrypt", CryptFail), WEXP(OpenProcessToken), WEXP(OpenThreadToken),
    WEXP(GetTokenInformation), WEXP(AllocateAndInitializeSid), WEXP(FreeSid), WEXP(CheckTokenMembership),
    WEXP(EventRegister),
    WEXP(ConvertSidToStringSidW), WEXP(ConvertSidToStringSidA), WEXP(ConvertStringSecurityDescriptorToSecurityDescriptorW),
    WEXP(ConvertStringSecurityDescriptorToSecurityDescriptorA), WEXP(SetNamedSecurityInfoW), WEXP(SetNamedSecurityInfoA),
    WEXP(GetNamedSecurityInfoW), WEXP(GetNamedSecurityInfoA), WEXP(IsValidSid), WEXP(GetLengthSid), WEXP(EqualSid),
    WEXP(CopySid),
    WEXPN("ImpersonateSelf", SecTrue), WEXPN("RevertToSelf", SecTrue), WEXP(DuplicateTokenEx),
    WEXPN("EventUnregister", EventZero), WEXPN("EventWriteTransfer", EventZero), WEXPN("EventWrite", EventZero),
    WEXPN("EventSetInformation", EventZero), WEXPN("EventEnabled", EventZero),
};
extern const wexport_t reg_exports[];
extern const int reg_nexports;
extern const wexport_t sec_exports[];
extern const int sec_nexports;
const wdll_t dll_advapi32 = { "advapi32.dll", advapi, sizeof(advapi) / sizeof(advapi[0]), reg_exports, &reg_nexports,
                              sec_exports, &sec_nexports };

/* ---- bcrypt ---------------------------------------------------------------------------------- */

static LONG WINAPI BCryptGenRandom(void *alg, BYTE *b, ULONG n, ULONG f) {
    (void)alg, (void)f;
    fill_random(b, n);
    return 0;
}
static LONG WINAPI BCryptNotSupported(void) { return (LONG)0xC00000BB; }   /* STATUS_NOT_SUPPORTED */

/* bcryptprimitives.dll's: Chromium's random numbers */
static BOOL WINAPI ProcessPrng(BYTE *b, SIZE_T n) {
    fill_random(b, n);
    return TRUE;
}

static const wexport_t bcrypt[] = {
    WEXP(BCryptGenRandom), WEXPN("BCryptOpenAlgorithmProvider", BCryptNotSupported),
    WEXPN("BCryptCloseAlgorithmProvider", EventZero), WEXP(ProcessPrng),
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

/* SHELLEXECUTEINFO (112 bytes): the verb, file, parameters and directory at
 * 16..40, the class at 72 (read with SEE_MASK_CLASSNAME, 1); the W call's
 * hInstApp (56) and hProcess (104) come back */
static BOOL WINAPI ShellExecuteExA(void *info) {
    static const int at[] = { 16, 24, 32, 40, 72 };
    uint8_t *a = info, w[112];
    WCHAR *s[5] = { 0 };
    BOOL r;
    if (!a || *(DWORD *)a < sizeof(w)) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memcpy(w, a, sizeof(w));
    for (int k = 0; k < 5; k++) {
        const char *p = *(const char **)(a + at[k]);
        if (k == 4 && !(*(DWORD *)(a + 4) & 1)) continue;
        s[k] = p ? utf8_to_w_dup(p) : 0;
        *(WCHAR **)(w + at[k]) = s[k];
    }
    r = ShellExecuteExW(w);
    memcpy(a + 56, w + 56, 8);
    memcpy(a + 104, w + 104, 8);
    for (int k = 0; k < 5; k++) free(s[k]);
    return r;
}

static int WINAPI SHCreateDirectoryExA(HWND w, const char *path, void *sa) {
    WCHAR *p = utf8_to_w_dup(path);
    int r = p ? SHCreateDirectoryExW(w, p, sa) : ERROR_PATH_NOT_FOUND;
    free(p);
    return r;
}

/* COM-style calls that have no object here say so, and give no pointer */
static HRESULT WINAPI SHCreateAssociationRegistration(const GUID *iid, void **out) {
    (void)iid;
    if (out) *out = 0;
    return (HRESULT)0x80004001;    /* E_NOTIMPL */
}
static void WINAPI SHChangeNotify(LONG e, UINT f, const void *a, const void *b) { (void)e, (void)f, (void)a, (void)b; }
static HRESULT WINAPI SHGetPropertyStoreForWindow(HWND h, const GUID *iid, void **out) {
    (void)h, (void)iid;
    if (out) *out = 0;
    return (HRESULT)0x80004001;
}
static HRESULT WINAPI ShellOutNotImpl(void *a, void *b, void **out) {
    (void)a, (void)b;
    if (out) *out = 0;
    return (HRESULT)0x80004001;
}

static void WINAPI DragAcceptFiles(HWND w, BOOL a) { (void)w, (void)a; }
static UINT WINAPI DragQueryFileW(HANDLE d, UINT i, WCHAR *f, UINT n) {
    (void)d, (void)i, (void)f, (void)n;
    return 0;
}
/* i 0xFFFFFFFF: how many files; no buffer: a name's length in bytes */
static UINT WINAPI DragQueryFileA(HANDLE d, UINT i, char *f, UINT n) {
    WCHAR w[1024];
    UINT r;
    if (i == 0xFFFFFFFF) return DragQueryFileW(d, i, 0, 0);
    r = DragQueryFileW(d, i, w, 1024);
    if (!r || r >= 1024) return 0;
    if (!f) return (UINT)w_to_utf8(w, -1, 0, 0) - 1;
    if (!n) return 0;
    w_to_utf8(w, -1, f, (int)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n));
    return (UINT)strlen(f);
}
static void WINAPI DragFinish(HANDLE d) { (void)d; }
static HRESULT WINAPI ShellNotImpl(void) { return (HRESULT)0x80004001; }   /* E_NOTIMPL */
static BOOL WINAPI ShellFalse(void) { return FALSE; }
static BOOL WINAPI ShellTrue(void) { return TRUE; }
static void *WINAPI ShellNull(void) { return 0; }

static const wexport_t shell[] = {
    WEXP(CommandLineToArgvW), WEXP(SHGetFolderPathW), WEXP(SHGetFolderPathA), WEXP(SHGetSpecialFolderPathW),
    WEXP(SHGetSpecialFolderPathA), WEXP(SHGetKnownFolderPath), WEXP(SHCreateDirectoryExW), WEXP(ShellExecuteW),
    WEXP(ShellExecuteA), WEXP(ShellExecuteExW), WEXP(ShellExecuteExA), WEXP(SHCreateDirectoryExA), WEXP(DragAcceptFiles),
    WEXP(DragQueryFileW), WEXP(DragQueryFileA), WEXPN("DragQueryFile", DragQueryFileA), WEXP(DragFinish),
    WEXPN("SHOpenFolderAndSelectItems", ShellNotImpl), WEXPN("SHParseDisplayName", ShellNotImpl),
    /* these ignore their arguments, so the A and W names share them */
    WEXPN("SHGetSpecialFolderLocation", ShellNotImpl), WEXPN("SHGetPathFromIDListW", ShellFalse),
    WEXPN("SHGetPathFromIDListA", ShellFalse), WEXPN("SHBrowseForFolderW", ShellNull), WEXPN("SHBrowseForFolderA", ShellNull),
    WEXPN("Shell_NotifyIconW", ShellTrue), WEXPN("Shell_NotifyIconA", ShellTrue), WEXPN("IsUserAnAdmin", ShellFalse),
    WEXPN("SHGetFileInfoW", ShellNull), WEXPN("SHGetFileInfoA", ShellNull), WEXPN("ExtractIconW", ShellNull),
    WEXPN("ExtractIconA", ShellNull), WEXPN("SHFileOperationW", ShellNotImpl), WEXPN("SHFileOperationA", ShellNotImpl),
    WEXPN("SHAppBarMessage", ShellNull), WEXPN("SHCreateItemFromParsingName", ShellNotImpl),
    WEXPN("SetCurrentProcessExplicitAppUserModelID", ShellNotImpl), WEXP(SHCreateAssociationRegistration),
    WEXP(SHChangeNotify), WEXP(SHGetPropertyStoreForWindow), WEXPN("SHCreateItemFromIDList", ShellOutNotImpl),
    WEXPN("SHGetDesktopFolder", ShellNotImpl), WEXPN("SHGetMalloc", ShellNotImpl),
    WEXPN("SHCreateDataObject", ShellNotImpl), WEXPO("IsUserAnAdmin", ShellFalse, 680),
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
static HRESULT WINAPI OleNotImpl(void) { return (HRESULT)0x80004001; } /* E_NOTIMPL */

/* ---- Windows Runtime: strings, and no runtime classes ---------------------------------------
 * An HSTRING is a length and its characters; one made by reference
 * (WindowsCreateStringReference) lives in the caller's HSTRING_HEADER. */
typedef struct {
    uint32_t       flags;       /* 1: a reference (not ours to free) */
    uint32_t       len;
    const WCHAR *buf;
    WCHAR        data[];
} hstr_t;

static HRESULT WINAPI WindowsCreateString(const WCHAR *src, uint32_t len, hstr_t **out) {
    hstr_t *h;
    if (!out) return (HRESULT)0x80070057;     /* E_INVALIDARG */
    *out = 0;
    if (!len) return 0;
    if (!src) return (HRESULT)0x80004003;     /* E_POINTER */
    h = malloc(sizeof(*h) + ((size_t)len + 1) * 2);
    if (!h) return (HRESULT)0x8007000E;       /* E_OUTOFMEMORY */
    h->flags = 0, h->len = len;
    memcpy(h->data, src, (size_t)len * 2);
    h->data[len] = 0;
    h->buf = h->data;
    *out = h;
    return 0;
}
static HRESULT WINAPI WindowsCreateStringReference(const WCHAR *src, uint32_t len, void *header, hstr_t **out) {
    hstr_t *h = header;          /* HSTRING_HEADER: 24 bytes, ours fits (16) */
    if (!out || !header) return (HRESULT)0x80070057;
    *out = 0;
    if (!len) return 0;
    h->flags = 1, h->len = len, h->buf = src;
    *out = h;
    return 0;
}
static HRESULT WINAPI WindowsDeleteString(hstr_t *h) {
    if (h && !(h->flags & 1)) free(h);
    return 0;
}
static const WCHAR *WINAPI WindowsGetStringRawBuffer(hstr_t *h, uint32_t *len) {
    static const WCHAR empty[1];
    if (len) *len = h ? h->len : 0;
    return h ? h->buf : empty;
}
static uint32_t WINAPI WindowsGetStringLen(hstr_t *h) { return h ? h->len : 0; }
static BOOL WINAPI WindowsIsStringEmpty(hstr_t *h) { return !h || !h->len; }
static HRESULT WINAPI WindowsDuplicateString(hstr_t *h, hstr_t **out) {
    return h ? WindowsCreateString(h->buf, h->len, out) : (*out = 0, 0);
}
/* no runtime class is registered */
static HRESULT WINAPI RoNoClass(void) { return (HRESULT)0x80040154; }    /* REGDB_E_CLASSNOTREG */

/* COM bits a program asks before using it */
static HRESULT WINAPI CoGetApartmentType(int *type, int *qual) {
    if (type) *type = 1;           /* APTTYPE_MTA */
    if (qual) *qual = 0;
    return 0;
}
static HRESULT WINAPI CoRegisterInitializeSpy(void *spy, uint64_t *cookie) {
    (void)spy;
    if (cookie) *cookie = 1;
    return 0;
}

static const wexport_t ole[] = {
    WEXP(CoInitializeEx), WEXP(CoInitialize), WEXP(CoUninitialize), WEXP(CoCreateInstance), WEXP(CoTaskMemAlloc),
    WEXP(CoTaskMemRealloc), WEXP(CoTaskMemFree), WEXP(CLSIDFromString), WEXP(IIDFromString), WEXP(StringFromGUID2),
    WEXP(CoCreateGuid), WEXP(PropVariantClear), WEXPN("OleInitialize", OleOk), WEXPN("OleUninitialize", OleOk),
    WEXPN("RegisterDragDrop", OleOk), WEXPN("RevokeDragDrop", OleOk), WEXPN("CoRegisterClassObject", OleFail),
    WEXPN("CoGetClassObject", CoCreateInstance), WEXPN("CoInitializeSecurity", OleOk),
    WEXPN("CoSetProxyBlanket", OleOk), WEXPN("CoWaitForMultipleHandles", OleFail),
    WEXPN("RoInitialize", OleOk), WEXPN("RoUninitialize", OleOk), WEXPN("RoGetActivationFactory", RoNoClass),
    WEXPN("RoActivateInstance", RoNoClass), WEXPN("RoGetAgileReference", OleNotImpl), WEXP(WindowsCreateString),
    WEXP(WindowsCreateStringReference), WEXP(WindowsDeleteString), WEXP(WindowsGetStringRawBuffer),
    WEXP(WindowsGetStringLen), WEXP(WindowsIsStringEmpty), WEXP(WindowsDuplicateString), WEXP(CoGetApartmentType),
    WEXP(CoRegisterInitializeSpy), WEXPN("CoRevokeInitializeSpy", OleOk), WEXPN("CoAllowSetForegroundWindow", OleOk),
    WEXPN("CoRegisterMessageFilter", OleOk), WEXPN("CreateStreamOnHGlobal", OleNotImpl),
    WEXPN("CoCreateFreeThreadedMarshaler", OleNotImpl), WEXPN("CoMarshalInterThreadInterfaceInStream", OleNotImpl),
    WEXPN("CoGetInterfaceAndReleaseStream", OleNotImpl), WEXPN("CoUnmarshalInterface", OleNotImpl),
    WEXPN("CoGetObject", OleNotImpl), WEXPN("DoDragDrop", OleNotImpl),
};
const wdll_t dll_ole32 = { "ole32.dll", ole, sizeof(ole) / sizeof(ole[0]) };

/* BSTRs: the length in bytes just before the text, the text 8-byte aligned
 * as 64-bit Windows has it (a 4-byte pad, then the length) */
static WCHAR *bstr_alloc(const void *bytes, UINT n) {
    uint8_t *b = malloc(8 + (size_t)n + 2);
    if (!b) return 0;
    *(uint32_t *)(b + 4) = n;
    if (bytes) memcpy(b + 8, bytes, n);
    else memset(b + 8, 0, n);
    b[8 + n] = b[8 + n + 1] = 0;
    return (WCHAR *)(b + 8);
}
static WCHAR *WINAPI SysAllocStringLen(const WCHAR *s, UINT n) { return bstr_alloc(s, n * 2); }
static WCHAR *WINAPI SysAllocString(const WCHAR *s) { return s ? SysAllocStringLen(s, (UINT)w_len(s)) : 0; }
static WCHAR *WINAPI SysAllocStringByteLen(const char *s, UINT n) { return bstr_alloc(s, n); }
static void WINAPI SysFreeString(WCHAR *s) {
    if (s) free((uint8_t *)s - 8);
}
static UINT WINAPI SysStringLen(const WCHAR *s) { return s ? ((const uint32_t *)s)[-1] / 2 : 0; }
static UINT WINAPI SysStringByteLen(const WCHAR *s) { return s ? ((const uint32_t *)s)[-1] : 0; }
static INT WINAPI SysReAllocStringLen(WCHAR **old, const WCHAR *s, UINT n) {
    WCHAR *w = SysAllocStringLen(s, n);
    if (!w) return FALSE;
    SysFreeString(*old);
    *old = w;
    return TRUE;
}
static INT WINAPI SysReAllocString(WCHAR **old, const WCHAR *s) {
    return SysReAllocStringLen(old, s, s ? (UINT)w_len(s) : 0);
}

/* VARIANTs (24 bytes): what they hold is freed and copied by its type */
enum { VT_EMPTY_ = 0, VT_BSTR_ = 8, VT_DISPATCH_ = 9, VT_UNKNOWN_ = 13, VT_ARRAY_ = 0x2000, VT_BYREF_ = 0x4000 };
typedef struct {
    uint16_t vt, r1, r2, r3;
    union {
        WCHAR *bstr;
        void  *unk;
        uint8_t raw[16];
    } u;
} variant_t;
_Static_assert(sizeof(variant_t) == 24, "VARIANT is 24 bytes");

static void com_unknown_call(void *unk, int slot) {
    if (unk) ((ULONG (WINAPI *)(void *))(*(void ***)unk)[slot])(unk);     /* 1 AddRef, 2 Release */
}
static void WINAPI VariantInit(void *v) { memset(v, 0, sizeof(variant_t)); }
static HRESULT WINAPI VariantClear(void *p) {
    variant_t *v = p;
    if (!v) return 0x80070057;                     /* E_INVALIDARG */
    if (!(v->vt & VT_BYREF_)) {
        if (v->vt == VT_BSTR_) SysFreeString(v->u.bstr);
        else if (v->vt == VT_DISPATCH_ || v->vt == VT_UNKNOWN_) com_unknown_call(v->u.unk, 2);
    }
    memset(v, 0, sizeof(*v));
    return 0;
}
static HRESULT WINAPI VariantCopy(void *dst, const void *src) {
    variant_t *d = dst;
    const variant_t *s = src;
    variant_t t;
    if (!d || !s) return 0x80070057;
    if (d == s) return 0;
    if ((s->vt & VT_ARRAY_) && !(s->vt & VT_BYREF_)) return 0x80004001;   /* SAFEARRAYs: not here */
    t = *s;
    if (!(t.vt & VT_BYREF_)) {
        if (t.vt == VT_BSTR_ && t.u.bstr) {
            t.u.bstr = SysAllocStringByteLen((const char *)s->u.bstr, SysStringByteLen(s->u.bstr));
            if (!t.u.bstr) return 0x8007000E;      /* E_OUTOFMEMORY */
        } else if (t.vt == VT_DISPATCH_ || t.vt == VT_UNKNOWN_) {
            com_unknown_call(t.u.unk, 1);
        }
    }
    VariantClear(d);
    *d = t;
    return 0;
}

static const wexport_t oleaut[] = {
    WEXPO("SysAllocString", SysAllocString, 2), WEXPO("SysReAllocString", SysReAllocString, 3),
    WEXPO("SysAllocStringLen", SysAllocStringLen, 4), WEXPO("SysReAllocStringLen", SysReAllocStringLen, 5),
    WEXPO("SysFreeString", SysFreeString, 6), WEXPO("SysStringLen", SysStringLen, 7),
    WEXPO("VariantInit", VariantInit, 8), WEXPO("VariantClear", VariantClear, 9),
    WEXPO("VariantCopy", VariantCopy, 10), WEXPO("SysStringByteLen", SysStringByteLen, 149),
    WEXPO("SysAllocStringByteLen", SysAllocStringByteLen, 150),
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
    WEXPN("ImmGetCompositionStringA", ImmZero), WEXPN("ImmSetCompositionStringA", ImmFalse),
    WEXPN("ImmGetCandidateListA", ImmZero),
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

/* ---- version: a file's version resource (VS_VERSIONINFO) ------------------------------------
 * GetFileVersionInfo copies the RT_VERSION resource; VerQueryValue walks its
 * tree: each node is wLength, wValueLength, wType, a key, then its value and
 * its children, each 32-bit aligned. */

static void *version_block(const WCHAR *wpath, DWORD *size) {
    char a[1100], up[1100];
    w_to_utf8(wpath, -1, a, sizeof(a));
    if (path_find(a, up, sizeof(up)) != 0) return 0;
    return pe_file_resource(up, (const WCHAR *)(uintptr_t)16, (const WCHAR *)(uintptr_t)1, size);   /* RT_VERSION, 1 */
}

static DWORD WINAPI GetFileVersionInfoSizeW(const WCHAR *path, DWORD *h) {
    DWORD size = 0;
    void *b = version_block(path, &size);
    if (h) *h = 0;
    if (!b) {
        wset_last_error(1813);    /* ERROR_RESOURCE_TYPE_NOT_FOUND */
        return 0;
    }
    free(b);
    return size * 2 + 4;          /* room for the ANSI copy, as Windows gives */
}

static BOOL WINAPI GetFileVersionInfoW(const WCHAR *path, DWORD h, DWORD len, void *data) {
    DWORD size = 0;
    void *b = version_block(path, &size);
    (void)h;
    if (!b) {
        wset_last_error(1813);
        return FALSE;
    }
    memset(data, 0, len);
    memcpy(data, b, size < len ? size : len);
    free(b);
    return TRUE;
}

static DWORD WINAPI GetFileVersionInfoSizeA(const char *path, DWORD *h) {
    WCHAR *w = utf8_to_w_dup(path);
    DWORD r = GetFileVersionInfoSizeW(w, h);
    free(w);
    return r;
}

static BOOL WINAPI GetFileVersionInfoA(const char *path, DWORD h, DWORD len, void *data) {
    WCHAR *w = utf8_to_w_dup(path);
    BOOL r = GetFileVersionInfoW(w, h, len, data);
    free(w);
    return r;
}

static DWORD WINAPI GetFileVersionInfoSizeExW(DWORD f, const WCHAR *path, DWORD *h) {
    (void)f;
    return GetFileVersionInfoSizeW(path, h);
}

static BOOL WINAPI GetFileVersionInfoExW(DWORD f, const WCHAR *path, DWORD h, DWORD len, void *data) {
    (void)f;
    return GetFileVersionInfoW(path, h, len, data);
}

static DWORD WINAPI GetFileVersionInfoSizeExA(DWORD f, const char *path, DWORD *h) {
    (void)f;
    return GetFileVersionInfoSizeA(path, h);
}

static BOOL WINAPI GetFileVersionInfoExA(DWORD f, const char *path, DWORD h, DWORD len, void *data) {
    (void)f;
    return GetFileVersionInfoA(path, h, len, data);
}

/* a node's key, value and first child */
static const uint8_t *vnode(const uint8_t *n, const WCHAR **key, const void **val, UINT *vlen, int *vtype, const uint8_t **kids, const uint8_t **end) {
    uint16_t len = ((const uint16_t *)n)[0], value_len = ((const uint16_t *)n)[1], type = ((const uint16_t *)n)[2];
    const WCHAR *k = (const WCHAR *)(n + 6);
    const uint8_t *p = (const uint8_t *)(k + w_len(k) + 1);
    p = (const uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
    *key = k;
    *val = p;
    *vlen = value_len;
    *vtype = type;
    p += type == 1 ? value_len * 2 : value_len;
    *kids = (const uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
    *end = n + len;
    return n;
}

static BOOL ver_query(const void *block, const WCHAR *sub, void **out, UINT *outlen) {
    const uint8_t *n = block, *kids, *end;
    const WCHAR *key, *p = sub;
    const void *val;
    UINT vlen;
    int vtype;
    vnode(n, &key, &val, &vlen, &vtype, &kids, &end);
    while (*p == '\\') p++;
    while (*p) {
        const WCHAR *e = p;
        const uint8_t *c;
        int found = 0;
        while (*e && *e != '\\') e++;
        for (c = kids; c + 6 < end && ((const uint16_t *)c)[0];) {
            const uint8_t *ck, *ce;
            const WCHAR *k;
            const void *v;
            UINT vl;
            int vt;
            vnode(c, &k, &v, &vl, &vt, &ck, &ce);
            if (w_len(k) == (size_t)(e - p)) {
                size_t i = 0;
                for (; i < (size_t)(e - p); i++) {
                    WCHAR x = k[i], y = p[i];
                    if (x >= 'A' && x <= 'Z') x += 32;
                    if (y >= 'A' && y <= 'Z') y += 32;
                    if (x != y) break;
                }
                if (i == (size_t)(e - p)) {
                    n = c, key = k, val = v, vlen = vl, vtype = vt, kids = ck, end = ce;
                    found = 1;
                    break;
                }
            }
            c = (const uint8_t *)(((uintptr_t)ce + 3) & ~(uintptr_t)3);
        }
        if (!found) return FALSE;
        p = *e ? e + 1 : e;
    }
    *out = (void *)val;
    if (outlen) *outlen = vlen;
    return vlen > 0 || vtype == 1;
}

static BOOL WINAPI VerQueryValueW(const void *block, const WCHAR *sub, void **out, UINT *len) {
    if (!block || !sub) return FALSE;
    return ver_query(block, sub, out, len);
}

static BOOL WINAPI VerQueryValueA(const void *block, const char *sub, void **out, UINT *len) {
    WCHAR *w = utf8_to_w_dup(sub ? sub : "\\");
    BOOL r = ver_query(block, w, out, len);
    int is_string = r && !strstr(sub ? sub : "", "VarFileInfo") && strcmp(sub ? sub : "\\", "\\");
    free(w);
    if (r && is_string) {
        /* the string in UTF-8, after the block (GetFileVersionInfoSize left room) */
        static __thread char tmp[1024];
        w_to_utf8(*out, -1, tmp, sizeof(tmp));
        *out = tmp;
        if (len) *len = (UINT)strlen(tmp) + 1;
    }
    return r;
}

static const wexport_t version[] = {
    WEXP(GetFileVersionInfoSizeW), WEXP(GetFileVersionInfoSizeA), WEXP(GetFileVersionInfoW), WEXP(GetFileVersionInfoA),
    WEXP(GetFileVersionInfoSizeExW), WEXP(GetFileVersionInfoExW), WEXP(GetFileVersionInfoSizeExA),
    WEXP(GetFileVersionInfoExA), WEXP(VerQueryValueW), WEXP(VerQueryValueA),
};
const wdll_t dll_version = { "version.dll", version, sizeof(version) / sizeof(version[0]) };
