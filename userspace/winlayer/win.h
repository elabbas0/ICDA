/* ICDA's Windows layer: what its parts share.
 *
 * icda-win is a Linux program (ICDA's Linux personality runs it) that loads
 * Windows programs (PE32+, x86-64) and gives them the Windows API from
 * builtin DLLs written here in C.  Builtin functions use the Windows
 * calling convention (WINAPI: GCC's ms_abi), so a program calls them
 * directly; callbacks into the program (window procedures, thread starts,
 * DllMain) are ms_abi function pointers.  Windows types are spelled with
 * fixed sizes: on Windows a long is 32 bits and wchar_t is 16. */
#ifndef WINLAYER_WIN_H
#define WINLAYER_WIN_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define WINAPI   __attribute__((ms_abi))
#define WCALLBACK __attribute__((ms_abi))

typedef int32_t  BOOL;
typedef uint8_t  BYTE, BOOLEAN, UCHAR;
typedef uint16_t WORD, USHORT, ATOM;
typedef int16_t  SHORT;
typedef uint32_t DWORD, UINT, ULONG;
typedef int32_t  LONG, INT, HRESULT;
typedef int64_t  LONGLONG, LONG_PTR, INT_PTR, LPARAM, LRESULT, SSIZE_T;
typedef uint64_t ULONGLONG, ULONG_PTR, UINT_PTR, DWORD_PTR, SIZE_T, WPARAM, DWORD64;
typedef uint16_t WCHAR;
typedef void    *HANDLE, *HMODULE, *HINSTANCE, *HWND, *HDC, *HGLRC, *HICON, *HCURSOR, *HBRUSH, *HMENU, *HKEY,
                *HGDIOBJ, *HBITMAP, *HFONT, *HPEN, *HRGN, *HMONITOR, *HGLOBAL, *HLOCAL, *HHOOK, *HRAWINPUT, *HIMC,
                *HKL, *HDEVNOTIFY, *HPALETTE, *HRSRC, *FARPROC;

#define TRUE  1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define MAX_PATH 260

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG cx, cy; } SIZE;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef union { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; } GUID;
typedef struct { DWORD LowPart; LONG HighPart; } LUID;
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;

/* error codes (GetLastError) */
#define ERROR_SUCCESS               0
#define ERROR_INVALID_FUNCTION      1
#define ERROR_FILE_NOT_FOUND        2
#define ERROR_PATH_NOT_FOUND        3
#define ERROR_TOO_MANY_OPEN_FILES   4
#define ERROR_ACCESS_DENIED         5
#define ERROR_INVALID_HANDLE        6
#define ERROR_NOT_ENOUGH_MEMORY     8
#define ERROR_INVALID_DATA          13
#define ERROR_OUTOFMEMORY           14
#define ERROR_NO_MORE_FILES         18
#define ERROR_HANDLE_EOF            38
#define ERROR_NOT_SUPPORTED         50
#define ERROR_FILE_EXISTS           80
#define ERROR_INVALID_PARAMETER     87
#define ERROR_CALL_NOT_IMPLEMENTED  120
#define ERROR_INSUFFICIENT_BUFFER   122
#define ERROR_INVALID_NAME          123
#define ERROR_MOD_NOT_FOUND         126
#define ERROR_PROC_NOT_FOUND        127
#define ERROR_DIR_NOT_EMPTY         145
#define ERROR_ALREADY_EXISTS        183
#define ERROR_ENVVAR_NOT_FOUND      203
#define ERROR_MORE_DATA             234
#define ERROR_NO_MORE_ITEMS         259
#define ERROR_TIMEOUT               1460
#define ERROR_CLASS_ALREADY_EXISTS  1410
#define ERROR_CLASS_DOES_NOT_EXIST  1411
#define ERROR_INVALID_WINDOW_HANDLE 1400

#define WAIT_OBJECT_0   0x00000000
#define WAIT_ABANDONED  0x00000080
#define WAIT_TIMEOUT    0x00000102
#define WAIT_FAILED     0xFFFFFFFF
#define INFINITE        0xFFFFFFFF

/* ---- the builtin DLLs --------------------------------------------------------- */

typedef struct {
    const char *name;
    void       *fn;
    uint16_t    ordinal;      /* 0: none */
} wexport_t;

typedef struct wdll {
    const char      *name;     /* lower case, with .dll */
    const wexport_t *exports;
    int              count;
    const wexport_t *more;     /* a second table (another file's), or 0 */
    const int       *nmore;
    const wexport_t *more2;    /* a third, or 0 */
    const int       *nmore2;
} wdll_t;

#define WEXP(f)        { #f, (void *)f, 0 }
#define WEXPN(n, f)    { n, (void *)f, 0 }
#define WEXPO(n, f, o) { n, (void *)f, o }

/* each dll_*.c defines one or more of these */
extern const wdll_t dll_kernel32, dll_ntdll, dll_msvcrt, dll_ucrt, dll_vcruntime, dll_user32, dll_gdi32,
    dll_opengl32, dll_winmm, dll_advapi32, dll_shell32, dll_ole32, dll_oleaut32, dll_imm32, dll_setupapi,
    dll_version, dll_ws2_32, dll_bcrypt, dll_cabinet, dll_dwrite, dll_comctl32, dll_misc;

/* the builtin for a DLL name (any case, with or without .dll), 0 if none */
const wdll_t *builtin_find(const char *dll);
void *builtin_proc(const wdll_t *d, const char *name, int ordinal);
void *ansi_proc(const char *dll, const char *name);                 /* ansi.c */

/* what Windows API functions return (apis.c): a missing function's failure */
int         api_kind(const char *name);
int         api_exported(const char *dll, const char *name);   /* Windows' DLL has it */
int         api_has_dll(const char *dll);                      /* a DLL of Windows' own */

/* bad pointers passed to the API fail the call (barrier.c) */
void *barrier_wrap(const char *dll, const char *name, int ordinal, void *fn);
int   barrier_recover(void *ucontext, uintptr_t fault_address);
const char *api_ordinal_name(const char *dll, int ordinal);
uint64_t    api_failure(const char *name, int *known);

/* ---- modules (loader.c) ------------------------------------------------------- */

typedef struct wmodule {
    struct wmodule *next;
    char            name[64];        /* base name, lower case */
    char            path[512];       /* where it came from (Unix path), "" for a builtin */
    uint8_t        *base;            /* the image (a builtin's is a small fake one) */
    uint64_t        size;
    const wdll_t   *builtin;
    int             refs;
    int             tls_index;       /* -1: none */
    int             inited;
    void           *entry;
} wmodule_t;

wmodule_t *ld_load(const char *name, const char *referrer_dir, int *err);  /* name: as imported or as LoadLibrary got it */
wmodule_t *ld_by_handle(HMODULE h);
wmodule_t *ld_by_name(const char *name);
wmodule_t *ld_main(void);
void      *ld_proc(wmodule_t *m, const char *name, int ordinal);
int        ld_init_all(void);                        /* DllMains and TLS callbacks of what is loaded */
void       ld_thread_attach(int attach);             /* DLL_THREAD_ATTACH / DETACH for the calling thread */
int        ld_run_exe(const char *unix_path, char *const *argv);   /* loads and starts the program */
wmodule_t *ld_modules(void);
void      *ld_resource(wmodule_t *m, const WCHAR *type, const WCHAR *name, DWORD *size);
int        ld_enum_resources(wmodule_t *m, const WCHAR *type, int (*cb)(void *ctx, const WCHAR *name), void *ctx);
void      *pe_file_resource(const char *upath, const WCHAR *type, const WCHAR *name, DWORD *size);

/* a child process (CreateProcess) */
typedef struct {
    int          pid;
    volatile int done;
    DWORD        code;
    int          exit_fd;       /* the child writes its 32-bit exit code here */
} wproc_t;

/* ---- threads and the TEB (thread.c) --------------------------------------------- */

typedef struct wthread wthread_t;
typedef DWORD (WCALLBACK *LPTHREAD_START_ROUTINE)(void *);

void      *teb_new(wthread_t *t, void *stack_base, void *stack_limit);   /* also sets GS */
void      *teb_current(void);
void      *peb_get(void);
wthread_t *thread_current(void);
void       thread_main_init(void *stack_base, void *stack_limit);
DWORD      thread_id(void);
void       wset_last_error(DWORD e);
DWORD      wget_last_error(void);
void      *tls_slot_get(DWORD i);

/* ---- handles (handles.c) --------------------------------------------------------- */

enum { H_FREE = 0, H_FILE, H_EVENT, H_MUTEX, H_SEMAPHORE, H_THREAD, H_FIND, H_PROCESS, H_HEAP, H_MAPPING, H_CONSOLE,
       H_SOCKET, H_OTHER };

typedef struct whandle {
    int      kind;
    int      refs;
    void    *obj;
    uint32_t access;            /* what the handle may do (sections); ~0: anything */
} whandle_t;

HANDLE     h_new(int kind, void *obj);
whandle_t *h_get(HANDLE h, int kind);    /* kind 0: any */
void       h_close(HANDLE h);
HANDLE     h_std(int which);             /* 0 in, 1 out, 2 err */

/* named objects (thread.c): ns is the kind whose names they share */
#define NS_TIMER 100
HANDLE h_named(int kind, int ns, const WCHAR *name, void *obj);   /* obj malloc'd; freed if the name is in use */
HANDLE h_open_named(int kind, int ns, const WCHAR *name);
void   named_ref(void *obj, int delta);                           /* a handle duplicated (+1) or closed (-1) */
HANDLE event_new(BOOL manual, BOOL initial, const WCHAR *name, int ns);

/* waitable objects: one lock and one condition for all, so any wait can
 * look at several objects at once */
void sync_lock(void);
void sync_unlock(void);
void sync_broadcast(void);
int  sync_wait_ms(DWORD ms);             /* 0, or ETIMEDOUT (ms: by the deadline sync_deadline set) */
void sync_deadline(DWORD ms);            /* the deadline of the next timed waits */
void win_thread_setup(void);             /* a thread of ours that calls Windows code: its TEB */
int  wobj_signaled(whandle_t *h, int consume);   /* -1: not waitable */

/* ---- text (wstr.c) ---------------------------------------------------------------- */

size_t  w_len(const WCHAR *s);
int     w_cmp(const WCHAR *a, const WCHAR *b);
int     w_icmp(const WCHAR *a, const WCHAR *b);
WCHAR  *w_dup(const WCHAR *s);
int     w_to_utf8(const WCHAR *s, int len, char *out, int cap);      /* len -1: to the NUL; returns bytes, with NUL */
int     utf8_to_w(const char *s, int len, WCHAR *out, int cap);      /* returns WCHARs, with NUL */
char   *w_to_utf8_dup(const WCHAR *s);                                /* malloc */
WCHAR  *utf8_to_w_dup(const char *s);                                 /* malloc */
int     a_icmp(const char *a, const char *b);
void    a_lower(char *s);

/* ---- paths (path.c) ---------------------------------------------------------------- */

/* "C:\Games\x.exe", "\Games", "x.dat": to the Unix path the layer opens */
int  path_to_unix(const char *wpath, char *out, size_t cap);
/* a Unix path to how a program sees it ("Z:\home\...", or "C:\..." inside the prefix) */
int  path_to_win(const char *upath, char *out, size_t cap);
const char *path_prefix(void);           /* the C: drive's folder */
void path_init(void);

/* ---- the window backend (backend.c): an ICDA window, or memory when headless ---------- */

typedef struct {
    int type;        /* BEV_* */
    int x, y;        /* the pointer, in window pixels */
    int dx, dy;      /* motion since the last event (all there is while the pointer is locked) */
    int buttons;     /* 1 left, 2 right, 4 middle */
    int wheel;       /* +1 away from the user, -1 towards */
    int scancode;    /* BEV_KEY: PC set-1 code; 0x100 marks an E0-prefixed key */
    int down;        /* BEV_KEY: pressed; BEV_FOCUS: focused */
    int ch;          /* BEV_CHAR: a Unicode character */
    int w, h;        /* BEV_RESIZE: the new size in pixels */
} bevent_t;

enum { BEV_NONE = 0, BEV_KEY, BEV_CHAR, BEV_MOUSE, BEV_FOCUS, BEV_RESIZE, BEV_CLOSE };

int       bk_open(const char *title, int w, int h);     /* 0 ok: one window per program */
int       bk_is_open(void);
void      bk_close(void);
uint32_t *bk_pixels(int *w, int *h);                     /* BGRA, top row first, w pixels a row */
void      bk_present(int x, int y, int w, int h);        /* w 0: all */
int       bk_poll(bevent_t *ev);                         /* 1 when one was taken */
void      bk_wait(int ms);                               /* until an event may be there, or ms */
void      bk_cursor(int shape);                          /* 0 arrow, 1 text, -1 hidden */
void      bk_screen(int *w, int *h);                     /* the display */
int       bk_headless(void);
void      bk_title(const char *title);
void      bk_input_mode(int raw_keys, int pointer_lock); /* games: key releases, relative mouse */
uint32_t  bk_window_id(void);                            /* for /dev/dialog (0: none) */
uint64_t  bk_frames(void);                               /* frames presented */

/* ---- logging ------------------------------------------------------------------------ */

extern int wtrace;     /* ICDA_WIN_TRACE: 1 calls of note, 2 every unimplemented call */
void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wfatal(const char *title, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
/* tells the user, in a dialog when the desktop is there (and waits for it) */
void wnotice(const char *title, const char *text);
/* an ICDA dialog: the button's index, -1 if none or no desktop */
int dialog_run(const char *kind, const char *title, const char *text, const char *buttons, uint32_t window);

/* ---- small helpers ------------------------------------------------------------------- */

uint64_t now_100ns(void);        /* since 1601, as FILETIME counts */
uint64_t mono_ns(void);
void     unix_to_filetime(int64_t sec, long nsec, FILETIME *ft);
DWORD    errno_to_win(int e);

/* for printf-style formatting with the Windows varargs */
typedef __builtin_ms_va_list ms_va_list;
int  ms_vformat(char *out, size_t cap, const char *fmt, ms_va_list ap, int wide_args);
int  ms_vformatw(WCHAR *out, size_t cap, const WCHAR *fmt, ms_va_list ap);
int  ms_vscan(const char *in, const char *fmt, ms_va_list ap);

/* a command line split as Windows C runtimes split it (argv and its strings malloc'ed) */
int split_cmdline(const char *cmd, char ***argv);

/* the program's command line and its arguments */
extern char   *g_cmdline_a;
extern WCHAR  *g_cmdline_w;
extern int     g_argc;
extern char  **g_argv_a;
extern WCHAR **g_argv_w;
extern char    g_exe_win[MAX_PATH * 2];    /* the program as Windows sees it */
extern char    g_exe_unix[1024];

#endif
