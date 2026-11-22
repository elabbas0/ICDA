/* t_console: the C runtime and kernel32 a console program uses (printf and
 * scanf, files, threads, waits, TLS, setjmp, the command line), its output
 * compared with expected_console.txt */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>
#include <math.h>

static int counter;
static CRITICAL_SECTION cs;
static __thread int tls_var = 5;
static jmp_buf jb;

static DWORD WINAPI worker(void *p) {
    for (int i = 0; i < 10000; i++) { EnterCriticalSection(&cs); counter++; LeaveCriticalSection(&cs); }
    tls_var += (int)(intptr_t)p;
    return tls_var;
}
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void at_exit_fn(void) { printf("atexit ran\n"); fflush(stdout); }
static void jumper(int n) { if (n > 3) longjmp(jb, n); jumper(n + 1); }

int main(int argc, char **argv) {
    printf("argc=%d argv0=%s\n", argc, strstr(argv[0], "t_console") ? "EXE" : argv[0]);
    for (int i = 1; i < argc; i++) printf("argv[%d]=[%s]\n", i, argv[i]);
    printf("ints: %d %ld %lld %I64d %u %x %X %o %05d|%-5d|%+d\n", -42, 123456L, -1234567890123LL, (long long)1 << 40, 4000000000u, 255, 255, 8, 42, 42, 42);
    printf("floats: %f %.3f %e %g %10.2f|%-10.1f|\n", 3.14159, 2.71828, 12345.678, 0.0001, 1.5, 2.5);
    printf("strings: %s %.3s %10s|%-10s| %c%c %ls %S\n", "hello", "abcdef", "right", "left", 'o', 'k', L"wide", L"wide2");
    printf("size_t: %zu\n", sizeof(void *));
    char buf[64];
    int n = snprintf(buf, 8, "%s", "truncate me");
    printf("snprintf=%d [%s]\n", n, buf);
    int a, b; char word[16]; double d;
    int got = sscanf("12 34 word 5.5", "%d %d %15s %lf", &a, &b, word, &d);
    printf("sscanf=%d %d %d %s %.1f\n", got, a, b, word, d);
    long l = strtol("2147483647", 0, 10); unsigned long ul = strtoul("-1", 0, 10);
    printf("strtol=%ld strtoul=%lu sizeof(long)=%d\n", l, ul, (int)sizeof(long));
    FILE *f = fopen("t_out.txt", "w"); fprintf(f, "line1\nline2 %d\n", 77); fclose(f);
    f = fopen("T_OUT.TXT", "r");
    char line[64]; int lines = 0; while (f && fgets(line, sizeof line, f)) lines++;
    printf("file lines=%d (case-blind open %s)\n", lines, f ? "ok" : "FAILED");
    if (f) fclose(f);
    HANDLE h = CreateFileA("t_out2.bin", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    DWORD w; WriteFile(h, "ABCDEFGH", 8, &w, 0); CloseHandle(h);
    h = CreateFileA("t_out2.bin", GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    char rb[16] = {0}; DWORD r; SetFilePointer(h, 2, 0, FILE_BEGIN); ReadFile(h, rb, 4, &r, 0);
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz); CloseHandle(h);
    printf("CreateFile read=%lu [%s] size=%lld\n", r, rb, sz.QuadPart);
    WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA("t_out*", &fd); int found = 0;
    if (fh != INVALID_HANDLE_VALUE) { do found++; while (FindNextFileA(fh, &fd)); FindClose(fh); }
    printf("FindFirstFile found=%d\n", found);
    { DWORD at = GetFileAttributesA("nope.txt"); DWORD er = GetLastError(); printf("missing file attrs=%lx err=%lu\n", at, er); }
    char mod[MAX_PATH]; GetModuleFileNameA(0, mod, MAX_PATH);
    printf("module=%s drive=%c:\n", strstr(mod, "t_console") ? "EXE" : mod, mod[0]);
    printf("cmdline has exe: %d\n", strstr(GetCommandLineA(), ".exe") != 0);
    InitializeCriticalSection(&cs);
    HANDLE th[4];
    for (int i = 0; i < 4; i++) th[i] = CreateThread(0, 0, worker, (void *)(intptr_t)(i + 1), 0, 0);
    WaitForMultipleObjects(4, th, TRUE, INFINITE);
    DWORD codes = 0;
    for (int i = 0; i < 4; i++) { DWORD c; GetExitCodeThread(th[i], &c); codes += c; CloseHandle(th[i]); }
    printf("threads counter=%d exitcodes=%lu main tls=%d\n", counter, codes, tls_var);
    HANDLE ev = CreateEventA(0, FALSE, FALSE, 0);
    printf("wait timeout=%lu\n", WaitForSingleObject(ev, 50));
    SetEvent(ev); printf("wait signaled=%lu\n", WaitForSingleObject(ev, 0));
    DWORD slot = TlsAlloc(); TlsSetValue(slot, (void *)0x1234); printf("tls slot=%p\n", TlsGetValue(slot));
    int v = setjmp(jb); if (v == 0) jumper(0); printf("longjmp got %d\n", v);
    int arr[] = {5, 3, 9, 1, 7}; qsort(arr, 5, sizeof(int), cmp);
    printf("qsort %d %d %d %d %d\n", arr[0], arr[1], arr[2], arr[3], arr[4]);
    volatile double one = 1.0, two = 2.0, half = 0.5, mh = -1.5;
    printf("math %.4f %.4f %.4f %.1f\n", sin(one), sqrt(two), pow(two, half), floor(mh));
    LARGE_INTEGER q1, q2, fq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&q1); Sleep(20); QueryPerformanceCounter(&q2);
    double ms = (q2.QuadPart - q1.QuadPart) * 1000.0 / fq.QuadPart;
    printf("sleep ok=%d\n", ms >= 19 && ms < 200);
    time_t t = time(0); struct tm *tm = gmtime(&t); printf("year ok=%d\n", tm->tm_year + 1900 >= 2024);
    WCHAR wbuf[32]; int wn = MultiByteToWideChar(CP_UTF8, 0, "h\xc3\xa9llo", -1, wbuf, 32);
    char back[32]; int bn = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, back, 32, 0, 0);
    printf("utf conv %d %d %s\n", wn, bn, strcmp(back, "h\xc3\xa9llo") == 0 ? "ok" : "BAD");
    void *va = VirtualAlloc(0, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); memset(va, 1, 1 << 20);
    MEMORY_BASIC_INFORMATION mbi; VirtualQuery(va, &mbi, sizeof mbi);
    printf("virtualalloc aligned=%d state=%lx\n", ((uintptr_t)va & 0xFFFF) == 0, mbi.State);
    VirtualFree(va, 0, MEM_RELEASE);
    char *m = malloc(10); m = realloc(m, 100000); free(m);
    printf("env PATH set=%d\n", getenv("PATH") != 0);
    SYSTEM_INFO si; GetSystemInfo(&si); printf("cpus>0=%d page=%lu\n", si.dwNumberOfProcessors > 0, si.dwPageSize);
    OSVERSIONINFOA ov = { sizeof ov }; GetVersionExA(&ov); printf("version %lu.%lu\n", ov.dwMajorVersion, ov.dwMinorVersion);
    atexit(at_exit_fn);
    printf("done\n");
    return 7;
}
