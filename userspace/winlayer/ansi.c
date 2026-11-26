/* The ANSI ("A") versions of functions the layer implements as Unicode ("W").
 *
 * Windows keeps one implementation of most string functions, the W one, and
 * its A function converts on the way in and out.  The ANSI code page here is
 * UTF-8 (GetACP says 65001), so an A string is UTF-8 and an A count is a
 * count of bytes.  Each function keeps its own rule for an output buffer that
 * is too small, as the A function on Windows does:
 *
 *   put_len    the length without the NUL when it fits, else the size needed
 *              with it, nothing written (GetLongPathNameA, most path calls)
 *   put_count  counts that include the NUL; 0 asks for the size; too small
 *              fails with ERROR_INSUFFICIENT_BUFFER (GetDateFormatA)
 *   put_cut    cut to fit, at a character boundary (K32GetModuleBaseNameA)
 *
 * The W call always gets a buffer big enough for any answer, so these rules
 * are applied to the A size, never the W one: a UTF-8 string is longer in
 * bytes than in UTF-16 units once it has anything past ASCII.
 *
 * Functions whose A version needs a DLL's own state (FindFirstFileExA, the
 * resource enumerations, GetClassInfoA, the security calls) are in that
 * DLL's file, beside their W version.  The ones here call only exported W
 * functions; builtin_proc() finds them through ansi_proc(). */
#define _GNU_SOURCE
#include "win.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define WN 32768     /* UTF-16 units: the longest path Windows has */

/* ---- conversions ------------------------------------------------------------------------ */

/* an A string as W (malloc), and 0 for 0 */
static WCHAR *aw(const char *s) { return s ? utf8_to_w_dup(s) : 0; }

/* a name that may be a number (MAKEINTRESOURCE) instead of a string */
static WCHAR *aw_id(const char *s) { return (uintptr_t)s >> 16 ? utf8_to_w_dup(s) : (WCHAR *)(uintptr_t)s; }
static void free_id(WCHAR *w) {
    if ((uintptr_t)w >> 16) free(w);
}

static int cap_of(DWORD n) { return n > INT_MAX ? INT_MAX : (int)n; }

static DWORD put_len(const WCHAR *w, char *buf, DWORD n) {
    DWORD need = (DWORD)w_to_utf8(w, -1, 0, 0);
    if (!buf || need > n) return need;
    w_to_utf8(w, -1, buf, cap_of(n));
    return need - 1;
}

static int put_count(const WCHAR *w, char *buf, int n) {
    int need = w_to_utf8(w, -1, 0, 0);
    if (!n) return need;
    if (n < 0 || !buf) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (need > n) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    w_to_utf8(w, -1, buf, n);
    return need;
}

static DWORD put_cut(const WCHAR *w, char *buf, DWORD n) {
    if (!buf || !n) return 0;
    w_to_utf8(w, -1, buf, cap_of(n));
    return (DWORD)strlen(buf);
}

static WCHAR *wbig(void) {
    WCHAR *w = calloc(WN, sizeof(WCHAR));
    if (!w) wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return w;
}

/* ---- kernel32 ----------------------------------------------------------------------------- */

BOOL   WINAPI CallNamedPipeW(const WCHAR *name, void *in, DWORD nin, void *out, DWORD nout, DWORD *got, DWORD to);
BOOL   WINAPI CopyFileW(const WCHAR *from, const WCHAR *to, BOOL fail_if_exists);
BOOL   WINAPI CopyFileExW(const WCHAR *from, const WCHAR *to, void *progress, void *data, BOOL *cancel, DWORD flags);
BOOL   WINAPI CreateHardLinkW(const WCHAR *to, const WCHAR *from, void *sa);
HANDLE WINAPI CreateMailslotW(const WCHAR *name, DWORD max, DWORD to, void *sa);
HANDLE WINAPI CreateMutexExW(void *sa, const WCHAR *name, DWORD flags, DWORD access);
HANDLE WINAPI CreateSemaphoreExW(void *sa, LONG initial, LONG max, const WCHAR *name, DWORD flags, DWORD access);
HANDLE WINAPI CreateWaitableTimerW(void *sa, BOOL manual, const WCHAR *name);
HANDLE WINAPI CreateWaitableTimerExW(void *sa, const WCHAR *name, DWORD flags, DWORD access);
BOOL   WINAPI EnumSystemLocalesW(BOOL (WCALLBACK *fn)(WCHAR *), DWORD flags);
HANDLE WINAPI FindFirstChangeNotificationW(const WCHAR *p, BOOL sub, DWORD f);
BOOL   WINAPI GetComputerNameExW(int type, WCHAR *buf, DWORD *n);
DWORD  WINAPI GetConsoleTitleW(WCHAR *buf, DWORD n);
int    WINAPI GetDateFormatW(DWORD lcid, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n);
int    WINAPI GetTimeFormatW(DWORD lcid, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n);
BOOL   WINAPI GetDiskFreeSpaceW(const WCHAR *dir, DWORD *spc, DWORD *bps, DWORD *freec, DWORD *totalc);
BOOL   WINAPI GetDiskFreeSpaceExW(const WCHAR *dir, uint64_t *avail, uint64_t *total, uint64_t *freeb);
DWORD  WINAPI GetFinalPathNameByHandleW(HANDLE h, WCHAR *out, DWORD n, DWORD flags);
DWORD  WINAPI GetLogicalDriveStringsW(DWORD n, WCHAR *buf);
DWORD  WINAPI GetLongPathNameW(const WCHAR *in, WCHAR *out, DWORD n);
DWORD  WINAPI GetShortPathNameW(const WCHAR *in, WCHAR *out, DWORD n);
BOOL   WINAPI GetStringTypeExW(DWORD lcid, DWORD type, const WCHAR *s, int n, WORD *out);
UINT   WINAPI GetSystemWindowsDirectoryW(WCHAR *buf, UINT n);
UINT   WINAPI GetSystemWow64DirectoryW(WCHAR *buf, UINT n);
UINT   WINAPI GetTempFileNameW(const WCHAR *dir, const WCHAR *prefix, UINT unique, WCHAR *out);
BOOL   WINAPI GetVolumeInformationW(const WCHAR *root, WCHAR *name, DWORD nn, DWORD *serial, DWORD *maxlen, DWORD *flags,
                                    WCHAR *fs, DWORD nfs);
BOOL   WINAPI GetVolumeNameForVolumeMountPointW(const WCHAR *mp, WCHAR *out, DWORD n);
BOOL   WINAPI GetVolumePathNameW(const WCHAR *path, WCHAR *out, DWORD n);
DWORD  WINAPI K32GetModuleBaseNameW(HANDLE p, HMODULE h, WCHAR *buf, DWORD n);
DWORD  WINAPI K32GetModuleFileNameExW(HANDLE p, HMODULE h, WCHAR *buf, DWORD n);
BOOL   WINAPI MoveFileExW(const WCHAR *from, const WCHAR *to, DWORD flags);
BOOL   WINAPI MoveFileWithProgressW(const WCHAR *from, const WCHAR *to, void *p, void *d, DWORD flags);
HANDLE WINAPI OpenEventW(DWORD a, BOOL i, const WCHAR *n);
HANDLE WINAPI OpenMutexW(DWORD a, BOOL i, const WCHAR *n);
BOOL   WINAPI PeekConsoleInputW(HANDLE h, void *rec, DWORD n, DWORD *got);
BOOL   WINAPI ReplaceFileW(const WCHAR *replaced, const WCHAR *replacement, const WCHAR *backup, DWORD f, void *e, void *r);
BOOL   WINAPI SetFileAttributesW(const WCHAR *name, DWORD a);
BOOL   WINAPI WaitNamedPipeW(const WCHAR *name, DWORD to);

static BOOL WINAPI CallNamedPipeA(const char *name, void *in, DWORD nin, void *out, DWORD nout, DWORD *got, DWORD to) {
    WCHAR *w = aw(name);
    BOOL r = CallNamedPipeW(w, in, nin, out, nout, got, to);
    free(w);
    return r;
}

static BOOL WINAPI CopyFileA(const char *from, const char *to, BOOL fail_if_exists) {
    WCHAR *f = aw(from), *t = aw(to);
    BOOL r = CopyFileW(f, t, fail_if_exists);
    free(f), free(t);
    return r;
}

static BOOL WINAPI CopyFileExA(const char *from, const char *to, void *progress, void *data, BOOL *cancel, DWORD flags) {
    WCHAR *f = aw(from), *t = aw(to);
    BOOL r = CopyFileExW(f, t, progress, data, cancel, flags);
    free(f), free(t);
    return r;
}

static BOOL WINAPI CreateHardLinkA(const char *to, const char *from, void *sa) {
    WCHAR *t = aw(to), *f = aw(from);
    BOOL r = CreateHardLinkW(t, f, sa);
    free(t), free(f);
    return r;
}

static HANDLE WINAPI CreateMailslotA(const char *name, DWORD max, DWORD to, void *sa) {
    WCHAR *w = aw(name);
    HANDLE h = CreateMailslotW(w, max, to, sa);
    free(w);
    return h;
}

static HANDLE WINAPI CreateMutexExA(void *sa, const char *name, DWORD flags, DWORD access) {
    WCHAR *w = aw(name);
    HANDLE h = CreateMutexExW(sa, w, flags, access);
    free(w);
    return h;
}

static HANDLE WINAPI CreateSemaphoreExA(void *sa, LONG initial, LONG max, const char *name, DWORD flags, DWORD access) {
    WCHAR *w = aw(name);
    HANDLE h = CreateSemaphoreExW(sa, initial, max, w, flags, access);
    free(w);
    return h;
}

static HANDLE WINAPI CreateWaitableTimerA(void *sa, BOOL manual, const char *name) {
    WCHAR *w = aw(name);
    HANDLE h = CreateWaitableTimerW(sa, manual, w);
    free(w);
    return h;
}

static HANDLE WINAPI CreateWaitableTimerExA(void *sa, const char *name, DWORD flags, DWORD access) {
    WCHAR *w = aw(name);
    HANDLE h = CreateWaitableTimerExW(sa, w, flags, access);
    free(w);
    return h;
}

/* the A callback, for the W enumeration this thread is in */
typedef BOOL (WCALLBACK *LOCALE_ENUMPROCA)(char *);
static __thread LOCALE_ENUMPROCA locale_fn;

static BOOL WCALLBACK locale_a(WCHAR *w) {
    char a[96];
    w_to_utf8(w, -1, a, sizeof(a));
    return locale_fn(a);
}

static BOOL WINAPI EnumSystemLocalesA(LOCALE_ENUMPROCA fn, DWORD flags) {
    LOCALE_ENUMPROCA outer = locale_fn;    /* a callback may enumerate again */
    BOOL r;
    if (!fn) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    locale_fn = fn;
    r = EnumSystemLocalesW(locale_a, flags);
    locale_fn = outer;
    return r;
}

static HANDLE WINAPI FindFirstChangeNotificationA(const char *p, BOOL sub, DWORD f) {
    WCHAR *w = aw(p);
    HANDLE h = FindFirstChangeNotificationW(w, sub, f);
    free(w);
    return h;
}

/* *n: the buffer's size in; the length without the NUL out, or on
 * ERROR_MORE_DATA the size needed with it */
static BOOL WINAPI GetComputerNameExA(int type, char *buf, DWORD *n) {
    WCHAR w[256];
    DWORD wn = 256, need;
    if (!n) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!GetComputerNameExW(type, w, &wn)) return FALSE;
    need = (DWORD)w_to_utf8(w, -1, 0, 0);
    if (!buf || need > *n) {
        *n = need;
        wset_last_error(ERROR_MORE_DATA);
        return FALSE;
    }
    w_to_utf8(w, -1, buf, cap_of(*n));
    *n = need - 1;
    return TRUE;
}

static DWORD WINAPI GetConsoleTitleA(char *buf, DWORD n) {
    WCHAR *w = wbig();
    DWORD r = 0;
    if (!w) return 0;
    if (GetConsoleTitleW(w, WN)) r = put_cut(w, buf, n);
    else if (buf && n) buf[0] = 0;
    free(w);
    return r;
}

typedef int (WINAPI *format_w_t)(DWORD, DWORD, const SYSTEMTIME *, const WCHAR *, WCHAR *, int);

static int format_a(format_w_t fw, DWORD lcid, DWORD f, const SYSTEMTIME *st, const char *fmt, char *buf, int n) {
    WCHAR *wf = aw(fmt), w[512];
    int r = fw(lcid, f, st, wf, w, 512);
    free(wf);
    return r ? put_count(w, buf, n) : 0;
}

static int WINAPI GetDateFormatA(DWORD lcid, DWORD f, const SYSTEMTIME *st, const char *fmt, char *buf, int n) {
    return format_a(GetDateFormatW, lcid, f, st, fmt, buf, n);
}

static int WINAPI GetTimeFormatA(DWORD lcid, DWORD f, const SYSTEMTIME *st, const char *fmt, char *buf, int n) {
    return format_a(GetTimeFormatW, lcid, f, st, fmt, buf, n);
}

static BOOL WINAPI GetDiskFreeSpaceA(const char *dir, DWORD *spc, DWORD *bps, DWORD *freec, DWORD *totalc) {
    WCHAR *w = aw(dir);
    BOOL r = GetDiskFreeSpaceW(w, spc, bps, freec, totalc);
    free(w);
    return r;
}

static BOOL WINAPI GetDiskFreeSpaceExA(const char *dir, uint64_t *avail, uint64_t *total, uint64_t *freeb) {
    WCHAR *w = aw(dir);
    BOOL r = GetDiskFreeSpaceExW(w, avail, total, freeb);
    free(w);
    return r;
}

static DWORD WINAPI GetFinalPathNameByHandleA(HANDLE h, char *out, DWORD n, DWORD flags) {
    WCHAR *w = wbig();
    DWORD r;
    if (!w) return 0;
    r = GetFinalPathNameByHandleW(h, w, WN, flags);
    r = r && r < WN ? put_len(w, out, n) : 0;
    free(w);
    return r;
}

/* "C:\\0Z:\\0\0": the length without the last NUL, or the size needed */
static DWORD WINAPI GetLogicalDriveStringsA(DWORD n, char *buf) {
    WCHAR w[256];
    DWORD r = GetLogicalDriveStringsW(256, w), need;
    if (!r || r >= 256) return 0;
    need = (DWORD)w_to_utf8(w, (int)r, 0, 0);
    if (!buf || need > n) return need;
    w_to_utf8(w, (int)r, buf, cap_of(n));
    return need - 1;
}

typedef DWORD (WINAPI *path_w_t)(const WCHAR *, WCHAR *, DWORD);

static DWORD path_a(path_w_t fw, const char *in, char *out, DWORD n) {
    WCHAR *wi, *w;
    DWORD r;
    if (!in) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    wi = aw(in), w = wbig();
    r = wi && w ? fw(wi, w, WN) : 0;
    r = r && r < WN ? put_len(w, out, n) : 0;
    free(wi), free(w);
    return r;
}

static DWORD WINAPI GetLongPathNameA(const char *in, char *out, DWORD n) { return path_a(GetLongPathNameW, in, out, n); }
static DWORD WINAPI GetShortPathNameA(const char *in, char *out, DWORD n) { return path_a(GetShortPathNameW, in, out, n); }

/* one type word per UTF-16 unit of the string, as Windows' A version */
static BOOL WINAPI GetStringTypeExA(DWORD lcid, DWORD type, const char *s, int n, WORD *out) {
    WCHAR *w;
    int wn;
    BOOL r;
    if (!s || !out) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (n < 0) n = (int)strlen(s) + 1;
    wn = utf8_to_w(s, n, 0, 0) - 1;
    w = malloc(((size_t)wn + 1) * sizeof(WCHAR));
    if (!w) {
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    utf8_to_w(s, n, w, wn + 1);
    r = GetStringTypeExW(lcid, type, w, wn, out);
    free(w);
    return r;
}

static UINT WINAPI GetSystemWindowsDirectoryA(char *buf, UINT n) {
    WCHAR w[MAX_PATH];
    UINT r = GetSystemWindowsDirectoryW(w, MAX_PATH);
    return r && r < MAX_PATH ? put_len(w, buf, n) : 0;
}

static UINT WINAPI GetSystemWow64DirectoryA(char *buf, UINT n) {
    WCHAR w[MAX_PATH];
    UINT r = GetSystemWow64DirectoryW(w, MAX_PATH);
    return r && r < MAX_PATH ? put_len(w, buf, n) : 0;
}

/* out holds MAX_PATH bytes, as on Windows */
static UINT WINAPI GetTempFileNameA(const char *dir, const char *prefix, UINT unique, char *out) {
    WCHAR *d = aw(dir), *p = aw(prefix), w[MAX_PATH];
    UINT r;
    if (!dir || !out) {
        free(d), free(p);
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    r = GetTempFileNameW(d, p, unique, w);
    if (r) w_to_utf8(w, -1, out, MAX_PATH);
    free(d), free(p);
    return r;
}

/* the names cut to fit, as Windows' A version */
static BOOL WINAPI GetVolumeInformationA(const char *root, char *name, DWORD nn, DWORD *serial, DWORD *maxlen,
                                         DWORD *flags, char *fs, DWORD nfs) {
    WCHAR *r = aw(root), wn[MAX_PATH + 1], wf[MAX_PATH + 1];
    BOOL ok = GetVolumeInformationW(r, name ? wn : 0, name ? MAX_PATH + 1 : 0, serial, maxlen, flags, fs ? wf : 0,
                                    fs ? MAX_PATH + 1 : 0);
    free(r);
    if (!ok) return FALSE;
    if (name) put_cut(wn, name, nn);
    if (fs) put_cut(wf, fs, nfs);
    return TRUE;
}

static BOOL WINAPI GetVolumeNameForVolumeMountPointA(const char *mp, char *out, DWORD n) {
    WCHAR *m = aw(mp), w[MAX_PATH];
    BOOL ok = GetVolumeNameForVolumeMountPointW(m, w, MAX_PATH);
    free(m);
    if (!ok) return FALSE;
    if (!out || (DWORD)w_to_utf8(w, -1, 0, 0) > n) {
        wset_last_error(206);     /* ERROR_FILENAME_EXCED_RANGE */
        return FALSE;
    }
    w_to_utf8(w, -1, out, cap_of(n));
    return TRUE;
}

static BOOL WINAPI GetVolumePathNameA(const char *path, char *out, DWORD n) {
    WCHAR *p, *w;
    BOOL ok;
    if (!path) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    p = aw(path), w = wbig();
    ok = p && w && GetVolumePathNameW(p, w, WN);
    if (ok && (!out || (DWORD)w_to_utf8(w, -1, 0, 0) > n)) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        ok = FALSE;
    }
    if (ok) w_to_utf8(w, -1, out, cap_of(n));
    free(p), free(w);
    return ok;
}

static DWORD WINAPI K32GetModuleBaseNameA(HANDLE p, HMODULE h, char *buf, DWORD n) {
    WCHAR w[MAX_PATH];
    DWORD r = K32GetModuleBaseNameW(p, h, w, MAX_PATH);
    return r && r < MAX_PATH ? put_cut(w, buf, n) : 0;
}

static DWORD WINAPI K32GetModuleFileNameExA(HANDLE p, HMODULE h, char *buf, DWORD n) {
    WCHAR *w = wbig();
    DWORD r;
    if (!w) return 0;
    r = K32GetModuleFileNameExW(p, h, w, WN);
    r = r && r < WN ? put_cut(w, buf, n) : 0;
    free(w);
    return r;
}

static BOOL WINAPI MoveFileExA(const char *from, const char *to, DWORD flags) {
    WCHAR *f = aw(from), *t = aw(to);
    BOOL r = MoveFileExW(f, t, flags);
    free(f), free(t);
    return r;
}

static BOOL WINAPI MoveFileWithProgressA(const char *from, const char *to, void *p, void *d, DWORD flags) {
    WCHAR *f = aw(from), *t = aw(to);
    BOOL r = MoveFileWithProgressW(f, t, p, d, flags);
    free(f), free(t);
    return r;
}

static HANDLE WINAPI OpenEventA(DWORD a, BOOL i, const char *n) {
    WCHAR *w = aw(n);
    HANDLE h = OpenEventW(a, i, w);
    free(w);
    return h;
}

static HANDLE WINAPI OpenMutexA(DWORD a, BOOL i, const char *n) {
    WCHAR *w = aw(n);
    HANDLE h = OpenMutexW(a, i, w);
    free(w);
    return h;
}

/* INPUT_RECORD: a key event's character is a WCHAR in W, a CHAR in A */
static BOOL WINAPI PeekConsoleInputA(HANDLE h, void *rec, DWORD n, DWORD *got) {
    BOOL r = PeekConsoleInputW(h, rec, n, got);
    if (r && rec && got)
        for (DWORD i = 0; i < *got && i < n; i++) {
            uint8_t *e = (uint8_t *)rec + i * 20;
            if (*(WORD *)e == 1) {                      /* KEY_EVENT */
                WCHAR c = *(WCHAR *)(e + 14);
                *(WCHAR *)(e + 14) = (WCHAR)(c < 0x80 ? c : '?');
            }
        }
    return r;
}

static BOOL WINAPI ReplaceFileA(const char *replaced, const char *replacement, const char *backup, DWORD f, void *e, void *r) {
    WCHAR *a = aw(replaced), *b = aw(replacement), *c = aw(backup);
    BOOL ok = ReplaceFileW(a, b, c, f, e, r);
    free(a), free(b), free(c);
    return ok;
}

static BOOL WINAPI SetFileAttributesA(const char *name, DWORD a) {
    WCHAR *w = aw(name);
    BOOL r = SetFileAttributesW(w, a);
    free(w);
    return r;
}

static BOOL WINAPI WaitNamedPipeA(const char *name, DWORD to) {
    WCHAR *w = aw(name);
    BOOL r = WaitNamedPipeW(w, to);
    free(w);
    return r;
}

/* ---- user32 -------------------------------------------------------------------------------- */

typedef LRESULT (WCALLBACK *WNDPROC_)(HWND, UINT, WPARAM, LPARAM);
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; DWORD priv; } MSG_;

DWORD   WINAPI CharUpperBuffW(WCHAR *s, DWORD n);
INT_PTR WINAPI DialogBoxParamW(HINSTANCE i, const WCHAR *t, HWND p, void *proc, LPARAM lp);
INT_PTR WINAPI DialogBoxIndirectParamW(HINSTANCE inst, const void *tmpl, HWND parent, void *proc, LPARAM lp);
HWND    WINAPI CreateDialogIndirectParamW(HINSTANCE i, const void *t, HWND p, void *proc, LPARAM lp);
int     WINAPI DrawTextA(HDC dc, const char *s, int n, RECT *r, UINT f);
BOOL    WINAPI EnumDisplaySettingsA(const char *dev, DWORD i, void *dm);
int     WINAPI GetKeyNameTextW(LONG lp, WCHAR *buf, int n);
BOOL    WINAPI GetKeyboardLayoutNameW(WCHAR *n);
BOOL    WINAPI IsCharAlphaW(WCHAR c);
BOOL    WINAPI IsCharAlphaNumericW(WCHAR c);
BOOL    WINAPI IsDialogMessageW(HWND h, MSG_ *m);
UINT    WINAPI MapVirtualKeyA(UINT code, UINT type);
int     WINAPI MessageBoxA(HWND h, const char *text, const char *caption, UINT type);
LRESULT WINAPI SendMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp);
HWND    WINAPI GetDlgItem(HWND h, int id);
BOOL    WINAPI SetWindowTextA(HWND h, const char *t);
SHORT   WINAPI VkKeyScanW(WCHAR c);
WCHAR   win_towupper(WCHAR c);

/* a UTF-8 string's previous character */
static char *WINAPI CharPrevA(const char *start, const char *s) {
    const char *p = s;
    if (!start || !s || s <= start) return (char *)start;
    do p--;
    while (p > start && ((unsigned char)*p & 0xC0) == 0x80);
    return (char *)p;
}

/* each character upper-cased in place, where its upper case has as many bytes */
static DWORD WINAPI CharUpperBuffA(char *s, DWORD n) {
    if (!s) return 0;
    for (DWORD i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        WCHAR w[3];
        char out[8];
        if (i + (DWORD)len > n) len = 1;
        for (int k = 1; k < len; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) len = 1;      /* not UTF-8: a byte at a time */
        if (len == 1) {
            if (c >= 'a' && c <= 'z') s[i] = (char)(c - 32);
            i++;
            continue;
        }
        if (utf8_to_w(s + i, len, w, 3) == 2) {       /* one UTF-16 unit */
            w[0] = win_towupper(w[0]);
            if (w_to_utf8(w, 1, out, sizeof(out)) == len + 1) memcpy(s + i, out, (size_t)len);
        }
        i += (DWORD)len;
    }
    return n;
}

static INT_PTR WINAPI DialogBoxParamA(HINSTANCE i, const char *t, HWND p, void *proc, LPARAM lp) {
    WCHAR *w = aw_id(t);
    INT_PTR r = DialogBoxParamW(i, w, p, proc, lp);
    free_id(w);
    return r;
}

/* a dialog template's texts are UTF-16 in the A and W calls alike */
static INT_PTR WINAPI DialogBoxIndirectParamA(HINSTANCE i, const void *t, HWND p, void *proc, LPARAM lp) {
    return DialogBoxIndirectParamW(i, t, p, proc, lp);
}
static HWND WINAPI CreateDialogIndirectParamA(HINSTANCE i, const void *t, HWND p, void *proc, LPARAM lp) {
    return CreateDialogIndirectParamW(i, t, p, proc, lp);
}

static int WINAPI DrawTextExA(HDC dc, char *s, int n, RECT *r, UINT f, void *params) {
    (void)params;
    return DrawTextA(dc, s, n, r, f);
}

static BOOL WINAPI EnumDisplaySettingsExA(const char *dev, DWORD i, void *dm, DWORD f) {
    (void)f;
    return EnumDisplaySettingsA(dev, i, dm);
}

static int WINAPI GetKeyNameTextA(LONG lp, char *buf, int n) {
    WCHAR w[64];
    if (n <= 0 || !GetKeyNameTextW(lp, w, 64)) {
        if (buf && n > 0) buf[0] = 0;
        return 0;
    }
    return (int)put_cut(w, buf, (DWORD)n);
}

/* KL_NAMELENGTH, 9 */
static BOOL WINAPI GetKeyboardLayoutNameA(char *buf) {
    WCHAR w[9];
    if (!buf || !GetKeyboardLayoutNameW(w)) return FALSE;
    w_to_utf8(w, -1, buf, 9);
    return TRUE;
}

/* a single byte past ASCII is no character in UTF-8 */
static BOOL WINAPI IsCharAlphaA(char c) { return (unsigned char)c < 0x80 && IsCharAlphaW((WCHAR)c); }
static BOOL WINAPI IsCharAlphaNumericA(char c) { return (unsigned char)c < 0x80 && IsCharAlphaNumericW((WCHAR)c); }

static BOOL WINAPI IsDialogMessageA(HWND h, MSG_ *m) { return IsDialogMessageW(h, m); }

static UINT WINAPI MapVirtualKeyExA(UINT code, UINT type, HKL l) {
    (void)l;
    return MapVirtualKeyA(code, type);
}

static int WINAPI MessageBoxExA(HWND h, const char *text, const char *caption, UINT type, WORD lang) {
    (void)lang;
    return MessageBoxA(h, text, caption, type);
}

/* the messages go as SendMessageA sends them: its texts are converted */
static LRESULT WINAPI SendDlgItemMessageA(HWND h, int id, UINT m, WPARAM wp, LPARAM lp) {
    return SendMessageA(GetDlgItem(h, id), m, wp, lp);
}

static LRESULT WINAPI SendMessageTimeoutA(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT f, UINT t, DWORD_PTR *res) {
    LRESULT r = SendMessageA(h, m, wp, lp);
    (void)f, (void)t;
    if (res) *res = (DWORD_PTR)r;
    return 1;
}

static BOOL WINAPI SendNotifyMessageA(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    SendMessageA(h, m, wp, lp);
    return TRUE;
}

static BOOL WINAPI SetDlgItemTextA(HWND h, int id, const char *t) { return SetWindowTextA(GetDlgItem(h, id), t); }

static SHORT WINAPI VkKeyScanA(char c) { return (unsigned char)c < 0x80 ? VkKeyScanW((WCHAR)c) : (SHORT)-1; }
static SHORT WINAPI VkKeyScanExA(char c, HKL l) {
    (void)l;
    return VkKeyScanA(c);
}

/* ---- gdi32 --------------------------------------------------------------------------------- */

int  WINAPI AddFontResourceExW(const WCHAR *n, DWORD f, void *r);
int  WINAPI EnumFontFamiliesExW(HDC dc, void *lf, void *fn, LPARAM lp, DWORD f);
BOOL WINAPI GetTextExtentExPointW(HDC dc, const WCHAR *s, int n, int max, int *fit, int *dx, SIZE *sz);

static int WINAPI AddFontResourceExA(const char *n, DWORD f, void *r) {
    WCHAR *w = aw(n);
    int c = AddFontResourceExW(w, f, r);
    free(w);
    return c;
}

/* LOGFONT: 28 bytes of numbers, then the face name: 32 WCHARs or 32 bytes */
#define LF_HEAD 28
#define LF_FACE 32

static void logfont_w(const uint8_t *a, uint8_t *w) {
    memcpy(w, a, LF_HEAD);
    utf8_to_w((const char *)a + LF_HEAD, (int)strnlen((const char *)a + LF_HEAD, LF_FACE), (WCHAR *)(w + LF_HEAD), LF_FACE);
}

static void logfont_a(const uint8_t *w, uint8_t *a) {
    memcpy(a, w, LF_HEAD);
    w_to_utf8((const WCHAR *)(w + LF_HEAD), -1, (char *)a + LF_HEAD, LF_FACE);
}

/* TEXTMETRIC: 44 bytes of LONGs, the four characters (WCHARs in W, bytes in
 * A), five BYTEs; 60 bytes in W, 56 in A.  NEWTEXTMETRICEX adds four DWORDs
 * and a FONTSIGNATURE (24 bytes) */
static void textmetric_a(const uint8_t *w, uint8_t *a, int ntm) {
    memcpy(a, w, 44);
    for (int i = 0; i < 4; i++) {
        WCHAR c = ((const WCHAR *)(w + 44))[i];
        a[44 + i] = (uint8_t)(c > 0xFF ? 0xFF : c);
    }
    memcpy(a + 48, w + 52, 5);
    if (ntm) memcpy(a + 56, w + 60, 16 + 24);
}

typedef int (WCALLBACK *FONTENUMPROCA)(const void *, const void *, DWORD, LPARAM);
typedef struct { FONTENUMPROCA fn; LPARAM lp; } font_enum_t;

/* ENUMLOGFONTEX: the LOGFONT, then the full name (64), style (32) and script (32) */
static int WCALLBACK font_a(const void *lfw, const void *tmw, DWORD type, LPARAM lp) {
    const font_enum_t *e = (const font_enum_t *)lp;
    const uint8_t *w = lfw;
    uint8_t a[60 + 64 + 32 + 32], tm[96];
    logfont_a(w, a);
    w_to_utf8((const WCHAR *)(w + 92), -1, (char *)a + 60, 64);
    w_to_utf8((const WCHAR *)(w + 92 + 128), -1, (char *)a + 124, 32);
    w_to_utf8((const WCHAR *)(w + 92 + 192), -1, (char *)a + 156, 32);
    memset(tm, 0, sizeof(tm));
    if (tmw) textmetric_a(tmw, tm, (type & 4) != 0);     /* TRUETYPE_FONTTYPE: NEWTEXTMETRICEX */
    return e->fn(a, tm, type, e->lp);
}

static int WINAPI EnumFontFamiliesExA(HDC dc, const void *lf, FONTENUMPROCA fn, LPARAM lp, DWORD f) {
    uint8_t w[LF_HEAD + LF_FACE * 2];
    font_enum_t e = { fn, lp };
    if (!fn) return 0;
    if (lf) logfont_w(lf, w);
    else memset(w, 0, sizeof(w));
    return EnumFontFamiliesExW(dc, w, (void *)font_a, (LPARAM)&e, f);
}

/* counts in bytes: a character's extent is in dx[] once for each of its bytes,
 * as Windows does for a character of two bytes */
static BOOL WINAPI GetTextExtentExPointA(HDC dc, const char *s, int n, int max, int *fit, int *dx, SIZE *sz) {
    WCHAR *w;
    int *wdx = 0, wn, wfit = 0;
    BOOL r;
    if (!s || n < 0 || !sz) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    wn = utf8_to_w(s, n, 0, 0) - 1;
    w = malloc(((size_t)wn + 1) * sizeof(WCHAR));
    if (dx) wdx = calloc((size_t)wn + 1, sizeof(int));
    if (!w || (dx && !wdx)) {
        free(w), free(wdx);
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    utf8_to_w(s, n, w, wn + 1);
    r = GetTextExtentExPointW(dc, w, wn, max, fit ? &wfit : 0, wdx, sz);
    if (r) {
        int bi = 0, wi = 0, fitted = 0;
        while (bi < n) {
            unsigned char c = (unsigned char)s[bi];
            int bl = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (bi + bl > n) bl = n - bi;
            wi += utf8_to_w(s + bi, bl, 0, 0) - 1;     /* this character's UTF-16 units */
            if (wi > wn) wi = wn;
            for (int k = 0; k < bl; k++)
                if (dx) dx[bi + k] = wi ? wdx[wi - 1] : 0;
            bi += bl;
            if (wi <= wfit) fitted = bi;
        }
        if (fit) *fit = fitted;
    }
    free(w), free(wdx);
    return r;
}

/* ---- advapi32: the registry ------------------------------------------------------------------ */

LONG WINAPI RegCopyTreeW(HKEY from, const WCHAR *sub, HKEY to);
LONG WINAPI RegDeleteKeyExW(HKEY h, const WCHAR *sub, DWORD sam, DWORD r);
LONG WINAPI RegDeleteKeyValueW(HKEY h, const WCHAR *sub, const WCHAR *name);
LONG WINAPI RegDeleteTreeW(HKEY h, const WCHAR *sub);
LONG WINAPI RegLoadKeyW(HKEY h, const WCHAR *sub, const WCHAR *file);
LONG WINAPI RegUnLoadKeyW(HKEY h, const WCHAR *sub);
LONG WINAPI RegEnumKeyExA(HKEY h, DWORD i, char *name, DWORD *nn, DWORD *r, char *cls, DWORD *cn, FILETIME *ft);
LONG WINAPI RegCreateKeyExA(HKEY h, const char *sub, DWORD r, char *cls, DWORD opt, DWORD sam, void *sa, HKEY *out, DWORD *disp);
LONG WINAPI RegSetValueExA(HKEY h, const char *name, DWORD res, DWORD type, const BYTE *data, DWORD size);
LONG WINAPI RegCloseKey(HKEY h);

static LONG WINAPI RegCopyTreeA(HKEY from, const char *sub, HKEY to) {
    WCHAR *w = aw(sub);
    LONG r = RegCopyTreeW(from, w, to);
    free(w);
    return r;
}

static LONG WINAPI RegDeleteKeyExA(HKEY h, const char *sub, DWORD sam, DWORD res) {
    WCHAR *w = aw(sub);
    LONG r = RegDeleteKeyExW(h, w, sam, res);
    free(w);
    return r;
}

static LONG WINAPI RegDeleteKeyValueA(HKEY h, const char *sub, const char *name) {
    WCHAR *s = aw(sub), *n = aw(name);
    LONG r = RegDeleteKeyValueW(h, s, n);
    free(s), free(n);
    return r;
}

static LONG WINAPI RegDeleteTreeA(HKEY h, const char *sub) {
    WCHAR *w = aw(sub);
    LONG r = RegDeleteTreeW(h, w);
    free(w);
    return r;
}

static LONG WINAPI RegEnumKeyA(HKEY h, DWORD i, char *name, DWORD n) { return RegEnumKeyExA(h, i, name, &n, 0, 0, 0, 0); }

static LONG WINAPI RegLoadKeyA(HKEY h, const char *sub, const char *file) {
    WCHAR *s = aw(sub), *f = aw(file);
    LONG r = RegLoadKeyW(h, s, f);
    free(s), free(f);
    return r;
}

static LONG WINAPI RegUnLoadKeyA(HKEY h, const char *sub) {
    WCHAR *w = aw(sub);
    LONG r = RegUnLoadKeyW(h, w);
    free(w);
    return r;
}

/* the data is the A kind too: a REG_SZ value is a UTF-8 string, which
 * RegSetValueExA converts */
static LONG WINAPI RegSetKeyValueA(HKEY h, const char *sub, const char *name, DWORD type, const void *data, DWORD size) {
    HKEY k = h;
    LONG r;
    if (sub && *sub) {
        r = RegCreateKeyExA(h, sub, 0, 0, 0, 0x0002 /* KEY_SET_VALUE */, 0, &k, 0);
        if (r) return r;
    }
    r = RegSetValueExA(k, name, 0, type, data, size);
    if (k != h) RegCloseKey(k);
    return r;
}

/* ---- the tables ------------------------------------------------------------------------------ */

#define X(f) WEXP(f)
static const wexport_t k32_a[] = {
    X(CallNamedPipeA), X(CopyFileA), X(CopyFileExA), X(CreateHardLinkA), X(CreateMailslotA), X(CreateMutexExA),
    X(CreateSemaphoreExA), X(CreateWaitableTimerA), X(CreateWaitableTimerExA), X(EnumSystemLocalesA),
    X(FindFirstChangeNotificationA), X(GetComputerNameExA), X(GetConsoleTitleA), X(GetDateFormatA), X(GetTimeFormatA),
    X(GetDiskFreeSpaceA), X(GetDiskFreeSpaceExA), X(GetFinalPathNameByHandleA), X(GetLogicalDriveStringsA),
    X(GetLongPathNameA), X(GetShortPathNameA), X(GetStringTypeExA), X(GetSystemWindowsDirectoryA),
    X(GetSystemWow64DirectoryA), X(GetTempFileNameA), X(GetVolumeInformationA), X(GetVolumeNameForVolumeMountPointA),
    X(GetVolumePathNameA), X(K32GetModuleBaseNameA), X(K32GetModuleFileNameExA), X(MoveFileExA), X(MoveFileWithProgressA),
    X(OpenEventA), X(OpenMutexA), X(PeekConsoleInputA), X(ReplaceFileA), X(SetFileAttributesA), X(WaitNamedPipeA),
};

static const wexport_t u32_a[] = {
    X(CharPrevA), X(CharUpperBuffA), X(DialogBoxParamA), X(DialogBoxIndirectParamA), X(CreateDialogIndirectParamA),
    X(DrawTextExA), X(EnumDisplaySettingsExA), X(GetKeyNameTextA), X(GetKeyboardLayoutNameA), X(IsCharAlphaA),
    X(IsCharAlphaNumericA), X(IsDialogMessageA), X(MapVirtualKeyExA), X(MessageBoxExA), X(SendDlgItemMessageA),
    X(SendMessageTimeoutA), X(SendNotifyMessageA), X(SetDlgItemTextA), X(VkKeyScanA), X(VkKeyScanExA),
    WEXPN("IsDialogMessage", IsDialogMessageA),
};

static const wexport_t g32_a[] = { X(AddFontResourceExA), X(EnumFontFamiliesExA), X(GetTextExtentExPointA) };

static const wexport_t adv_a[] = {
    X(RegCopyTreeA), X(RegDeleteKeyExA), X(RegDeleteKeyValueA), X(RegDeleteTreeA), X(RegEnumKeyA), X(RegLoadKeyA),
    X(RegSetKeyValueA), X(RegUnLoadKeyA),
};

static const struct {
    const char      *dll;
    const wexport_t *ex;
    int              n;
} tables[] = {
    { "kernel32.dll", k32_a, sizeof(k32_a) / sizeof(k32_a[0]) },     /* kernelbase's too */
    { "user32.dll", u32_a, sizeof(u32_a) / sizeof(u32_a[0]) },
    { "gdi32.dll", g32_a, sizeof(g32_a) / sizeof(g32_a[0]) },
    { "advapi32.dll", adv_a, sizeof(adv_a) / sizeof(adv_a[0]) },
};

/* a builtin DLL's A function from here, or 0 */
void *ansi_proc(const char *dll, const char *name) {
    if (!dll || !name) return 0;
    for (size_t t = 0; t < sizeof(tables) / sizeof(tables[0]); t++) {
        if (strcmp(tables[t].dll, dll)) continue;
        for (int i = 0; i < tables[t].n; i++)
            if (!strcmp(tables[t].ex[i].name, name)) return tables[t].ex[i].fn;
    }
    return 0;
}
