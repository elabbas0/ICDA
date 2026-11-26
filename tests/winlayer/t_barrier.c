/* A bad pointer passed to the Windows API fails the call, as on Windows,
 * instead of stopping the program: the call returns its failure with
 * ERROR_NOACCESS.  Also: what a call through the barrier costs.
 * Prints one line a check; a line starting FAIL says what was wrong. */
#include <windows.h>
#include <stdio.h>

static int fails;
static void check(const char *what, int ok, unsigned long long got) {
    printf("%s %s: 0x%llx\n", ok ? "ok" : "FAIL", what, got);
    if (!ok) fails++;
}

int main(void) {
    RECT r = { 1, 2, 3, 4 };
    DWORD n = 0, err;
    HANDLE h;
    BOOL b;
    char path[MAX_PATH];
    LARGE_INTEGER t0, t1, f;

    SetLastError(0);
    b = GetCursorPos(NULL);
    err = GetLastError();
    check("GetCursorPos(NULL) fails", b == FALSE && err == ERROR_NOACCESS, err);
    check("EqualRect(NULL, ...) is FALSE", EqualRect(NULL, &r) == FALSE, 0);
    SetLastError(0);
    h = CreateFileW((const WCHAR *)0x1234, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    err = GetLastError();
    check("CreateFileW(bad name) is INVALID_HANDLE_VALUE", h == INVALID_HANDLE_VALUE && err == ERROR_NOACCESS, err);
    GetTempPathA(MAX_PATH, path);
    lstrcatA(path, "barrier.txt");
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(h, "hello", 5, &n, NULL);
    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    SetLastError(0);
    b = ReadFile(h, (void *)1, 5, &n, NULL);
    err = GetLastError();
    check("ReadFile into a bad buffer fails", b == FALSE && err == ERROR_NOACCESS, err);
    CloseHandle(h);
    DeleteFileA(path);
    /* and the program goes on, its calls working */
    check("GetCursorPos(&p) still works", GetCursorPos((POINT *)&r) != FALSE, 0);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 1000000; i++) GetVersion();
    QueryPerformanceCounter(&t1);
    printf("a call through the barrier: %.0f ns\n", (double)(t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / 1000000);
    printf("done, %d failed\n", fails);
    return fails;
}
