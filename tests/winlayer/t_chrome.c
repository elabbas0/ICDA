/* Windows behaviours that running Google Chrome under icda-win turned up,
 * each as Windows has it:
 *   one-time initialisation that starts another (BoringSSL);
 *   a named pipe whose client connected first: ConnectNamedPipe says FALSE
 *     and ERROR_PIPE_CONNECTED (Mojo's channels);
 *   a read-only duplicate of a section stays read-only, and NtQuerySection
 *     answers (Chrome's shared memory);
 *   a file cannot be cut short under a mapped view (SQLite);
 *   a non-blocking connect is WSAEWOULDBLOCK, not WSAEINPROGRESS;
 *   comctl32: InitCommonControlsEx and window subclassing.
 * Prints one line a check; a line starting FAIL says what was wrong. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, unsigned long long got, unsigned long long want) {
    printf("%s %s: 0x%llx\n", got == want ? "ok" : "FAIL", what, got);
    if (got != want) fails++;
}

/* ---- INIT_ONCE ------------------------------------------------------------------------- */

static INIT_ONCE outer = INIT_ONCE_STATIC_INIT, inner = INIT_ONCE_STATIC_INIT, flaky = INIT_ONCE_STATIC_INIT;
static int inner_runs, outer_runs, flaky_runs;

static BOOL CALLBACK run_inner(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o, (void)p;
    inner_runs++;
    *ctx = (PVOID)(ULONG_PTR)0x40;
    return TRUE;
}

static BOOL CALLBACK run_outer(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    PVOID c = 0;
    (void)o, (void)p;
    outer_runs++;
    InitOnceExecuteOnce(&inner, run_inner, 0, &c);        /* another one, from inside */
    *ctx = c;
    return TRUE;
}

static BOOL CALLBACK run_flaky(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o, (void)p, (void)ctx;
    return ++flaky_runs > 1;                               /* fails the first time */
}

static void init_once(void) {
    PVOID c = 0;
    BOOL pending = FALSE;
    INIT_ONCE manual = INIT_ONCE_STATIC_INIT;
    check("InitOnceExecuteOnce that starts another", InitOnceExecuteOnce(&outer, run_outer, 0, &c), TRUE);
    check("  the inner one's context", (ULONG_PTR)c, 0x40);
    InitOnceExecuteOnce(&outer, run_outer, 0, &c);
    check("  each ran once", outer_runs == 1 && inner_runs == 1, 1);
    check("InitOnceExecuteOnce failing", InitOnceExecuteOnce(&flaky, run_flaky, 0, 0), FALSE);
    check("  is tried again", InitOnceExecuteOnce(&flaky, run_flaky, 0, 0) && flaky_runs == 2, 1);
    check("InitOnceBeginInitialize", InitOnceBeginInitialize(&manual, 0, &pending, 0) && pending, 1);
    check("InitOnceComplete", InitOnceComplete(&manual, 0, (PVOID)(ULONG_PTR)0x80), TRUE);
    check("  then done", InitOnceBeginInitialize(&manual, INIT_ONCE_CHECK_ONLY, &pending, &c) && !pending &&
                             (ULONG_PTR)c == 0x80, 1);
}

/* ---- a named pipe --------------------------------------------------------------------------- */

static void named_pipe(void) {
    char name[80], buf[8] = "";
    HANDLE server, client;
    DWORD n = 0;
    BOOL r;
    snprintf(name, sizeof(name), "\\\\.\\pipe\\icda-test-%lu", GetCurrentProcessId());
    server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1, 4096, 4096, 5000, NULL);
    check("CreateNamedPipe", server != INVALID_HANDLE_VALUE, 1);
    client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    check("the client opens it", client != INVALID_HANDLE_VALUE, 1);
    SetLastError(0);
    r = ConnectNamedPipe(server, NULL);
    check("ConnectNamedPipe after the client: FALSE", r, FALSE);
    check("  and ERROR_PIPE_CONNECTED", GetLastError(), ERROR_PIPE_CONNECTED);
    WriteFile(client, "mojo", 4, &n, NULL);
    check("the bytes come through", ReadFile(server, buf, 4, &n, NULL) && n == 4 && !memcmp(buf, "mojo", 4), 1);
    CloseHandle(client), CloseHandle(server);
}

/* ---- sections --------------------------------------------------------------------------------- */

typedef LONG (NTAPI *NtQuerySection_t)(HANDLE, int, void *, ULONG, ULONG *);
typedef struct { void *base; ULONG attributes; LARGE_INTEGER size; } SECTION_BASIC;

static void sections(void) {
    HANDLE rw = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 8192, NULL), ro = 0, back = 0;
    NtQuerySection_t query = (NtQuerySection_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySection");
    SECTION_BASIC info;
    char *w, *r;
    check("CreateFileMapping of memory", rw != NULL, 1);
    check("a read-only duplicate", DuplicateHandle(GetCurrentProcess(), rw, GetCurrentProcess(), &ro,
                                                   FILE_MAP_READ | SECTION_QUERY, FALSE, 0), TRUE);
    SetLastError(0);
    check("  cannot be made writable again", DuplicateHandle(GetCurrentProcess(), ro, GetCurrentProcess(), &back,
                                                             FILE_MAP_WRITE, FALSE, 0), FALSE);
    check("  ERROR_ACCESS_DENIED", GetLastError(), ERROR_ACCESS_DENIED);
    check("  nor mapped for writing", MapViewOfFile(ro, FILE_MAP_WRITE, 0, 0, 0) == NULL, 1);
    w = MapViewOfFile(rw, FILE_MAP_WRITE, 0, 0, 0);
    r = MapViewOfFile(ro, FILE_MAP_READ, 0, 0, 0);
    if (w) strcpy(w, "shared");
    check("  its view sees the writable one's writes", w && r && !strcmp(r, "shared"), 1);
    memset(&info, 0, sizeof(info));
    check("NtQuerySection", query && query(ro, 0, &info, sizeof(info), NULL) == 0, 1);
    check("  its size", (unsigned long long)info.size.QuadPart, 8192);
    check("  memory, not an image", info.attributes, SEC_COMMIT);
    if (w) UnmapViewOfFile(w);
    if (r) UnmapViewOfFile(r);
    CloseHandle(ro), CloseHandle(rw);
}

static void mapped_file(void) {
    char tmp[MAX_PATH], path[MAX_PATH], data[8192];
    HANDLE f, m;
    void *view;
    DWORD n;
    GetTempPathA(sizeof(tmp), tmp);
    GetTempFileNameA(tmp, "map", 0, path);
    f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    memset(data, 'x', sizeof(data));
    WriteFile(f, data, sizeof(data), &n, NULL);
    m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    view = m ? MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    check("a file's view", view != NULL, 1);
    SetFilePointer(f, 4096, NULL, FILE_BEGIN);
    SetLastError(0);
    check("SetEndOfFile under the view", SetEndOfFile(f), FALSE);
    check("  ERROR_USER_MAPPED_FILE", GetLastError(), ERROR_USER_MAPPED_FILE);
    if (view) UnmapViewOfFile(view);
    CloseHandle(m);
    check("  with the view gone", SetEndOfFile(f) && GetFileSize(f, NULL) == 4096, 1);
    CloseHandle(f);
    DeleteFileA(path);
}

/* ---- sockets ---------------------------------------------------------------------------------- */

static void nonblocking_connect(void) {
    WSADATA wsa;
    SOCKET l, c;
    struct sockaddr_in a;
    int len = sizeof(a), r, e;
    u_long on = 1;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    l = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(l, (struct sockaddr *)&a, sizeof(a));
    listen(l, 1);
    getsockname(l, (struct sockaddr *)&a, &len);
    c = socket(AF_INET, SOCK_STREAM, 0);
    ioctlsocket(c, FIONBIO, &on);
    r = connect(c, (struct sockaddr *)&a, sizeof(a));
    e = r ? WSAGetLastError() : 0;
    check("a non-blocking connect: done or WSAEWOULDBLOCK", r == 0 || e == WSAEWOULDBLOCK, 1);
    closesocket(c), closesocket(l);
}

/* ---- comctl32 --------------------------------------------------------------------------------- */

static int seen_by_a, seen_by_b, seen_by_window, order_ok;

static LRESULT CALLBACK window_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_USER + 1) {
        seen_by_window++;
        return 7;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static LRESULT CALLBACK sub_a(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    if (m == WM_USER + 1) {
        seen_by_a++;
        if (ref != 0xA) return -1;
        order_ok = order_ok && seen_by_b == seen_by_a;          /* b, the newer, came first */
    }
    (void)id;
    return DefSubclassProc(h, m, wp, lp);
}

static LRESULT CALLBACK sub_b(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id, (void)ref;
    if (m == WM_USER + 1) seen_by_b++;
    return DefSubclassProc(h, m, wp, lp) + 1;
}

static void comctl(void) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES }, bad = { 0, ICC_WIN95_CLASSES };
    WNDCLASSW wc = { 0 };
    HWND h;
    DWORD_PTR ref = 0;
    LRESULT r;
    check("InitCommonControlsEx", InitCommonControlsEx(&icc), TRUE);
    check("  a wrong size fails", InitCommonControlsEx(&bad), FALSE);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"IcdaSubclassed";
    RegisterClassW(&wc);
    h = CreateWindowW(L"IcdaSubclassed", L"", 0, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    check("SetWindowSubclass", SetWindowSubclass(h, sub_a, 1, 0xA) && SetWindowSubclass(h, sub_b, 2, 0xB), TRUE);
    order_ok = 1;
    r = SendMessageW(h, WM_USER + 1, 0, 0);
    check("  the message through both, newest first, then the window", r == 8 && seen_by_a == 1 && seen_by_b == 1 &&
                                                                            seen_by_window == 1 && order_ok, 1);
    check("GetWindowSubclass", GetWindowSubclass(h, sub_a, 1, &ref) && ref == 0xA, 1);
    check("RemoveWindowSubclass", RemoveWindowSubclass(h, sub_b, 2) && RemoveWindowSubclass(h, sub_a, 1), TRUE);
    r = SendMessageW(h, WM_USER + 1, 0, 0);
    check("  the window's own procedure again", r == 7 && seen_by_a == 1 && seen_by_window == 2 &&
                                                    GetWindowLongPtrW(h, GWLP_WNDPROC) == (LONG_PTR)window_proc, 1);
    DestroyWindow(h);
}

int main(void) {
    init_once();
    named_pipe();
    sections();
    mapped_file();
    nonblocking_connect();
    comctl();
    printf("done, %d failed\n", fails);
    return fails != 0;
}
