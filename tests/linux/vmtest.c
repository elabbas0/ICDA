/* Memory on ICDA's Linux personality: what WebKit / JavaScriptCore do with
 * mmap.  Prints PASS / FAIL per check and "vmtest: ALL PASS" at the end. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (cond) printf("PASS %s\n", what); else { printf("FAIL %s\n", what); failures++; } fflush(stdout); } while (0)

int main(void) {
    /* 1: a huge reservation, then a piece of it made usable */
    {
        size_t big = 64ULL << 30;
        char *r = mmap(0, big, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        int ok = r != MAP_FAILED;
        if (ok) {
            char *mid = r + (32ULL << 30);
            ok = mprotect(mid, 1 << 20, PROT_READ | PROT_WRITE) == 0;
            if (ok) {
                for (int i = 0; i < (1 << 20); i += 4096) mid[i] = (char)i;
                for (int i = 0; i < (1 << 20) && ok; i += 4096) ok = mid[i] == (char)i;
            }
            ok = ok && munmap(r, big) == 0;
        }
        CHECK(ok, "64 GB reservation, 1 MB used inside");
    }

    /* 2: lazy anonymous memory reads as zeroes */
    {
        size_t n = 16 << 20;
        unsigned char *a = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int ok = a != MAP_FAILED;
        for (size_t i = 0; ok && i < n; i += 65536) ok = a[i] == 0;
        for (size_t i = 0; ok && i < n; i += 65536) a[i] = 0xA5;
        for (size_t i = 0; ok && i < n; i += 65536) ok = a[i] == 0xA5;
        munmap(a, n);
        CHECK(ok, "16 MB anonymous, zero-filled on touch");
    }

    /* 3: a file mapped privately */
    {
        const char *path = "vmtest.tmp";
        char text[9000];
        int fd, ok;
        for (int i = 0; i < (int)sizeof text; i++) text[i] = (char)('a' + i % 26);
        fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        ok = fd >= 0 && write(fd, text, sizeof text) == (ssize_t)sizeof text;
        if (ok) {
            char *m = mmap(0, sizeof text, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
            char *m2 = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 8192);
            ok = m != MAP_FAILED && m2 != MAP_FAILED && memcmp(m, text, sizeof text) == 0 && m2[0] == text[8192];
            if (ok) {
                char back;
                m[0] = 'Z';          /* private: the file keeps its byte */
                ok = pread(fd, &back, 1, 0) == 1 && back == 'a';
            }
        }
        if (fd >= 0) close(fd);
        unlink(path);
        CHECK(ok, "file mapping (with offset, private writes)");
    }

    /* 4: mremap grows a mapping and keeps its contents */
    {
        size_t n = 1 << 20;
        char *a = mmap(0, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        char *guard = mmap(a + n, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        char *b;
        int ok;
        for (size_t i = 0; i < n; i += 4096) a[i] = (char)(i >> 12);
        b = mremap(a, n, 8 * n, MREMAP_MAYMOVE);     /* the guard page forces a move */
        ok = b != MAP_FAILED && b != a && guard != MAP_FAILED;
        for (size_t i = 0; ok && i < n; i += 4096) ok = b[i] == (char)(i >> 12);
        if (ok) b[8 * n - 1] = 1;
        CHECK(ok, "mremap moves and keeps data");
        munmap(b, 8 * n);
        munmap(guard, 4096);
    }

    /* 5: realloc of big blocks (musl uses mremap) */
    {
        char *p = malloc(4 << 20);
        int ok = p != 0;
        if (ok) { memset(p, 7, 4 << 20); p = realloc(p, 64 << 20); ok = p && p[(4 << 20) - 1] == 7; }
        if (ok) p[(64 << 20) - 1] = 3;
        free(p);
        CHECK(ok, "realloc 4 MB -> 64 MB");
    }

    /* 6: MADV_DONTNEED gives the memory back (zeroes next time) */
    {
        char *a = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int ok;
        memset(a, 9, 65536);
        ok = madvise(a, 65536, MADV_DONTNEED) == 0 && a[0] == 0 && a[65535] == 0;
        munmap(a, 65536);
        CHECK(ok, "madvise DONTNEED");
    }

    /* 7: JIT: write code, then run it */
    {
        static const unsigned char code[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };   /* mov eax, 42; ret */
        unsigned char *x = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int ok = x != MAP_FAILED;
        if (ok) {
            memcpy(x, code, sizeof code);
            ok = mprotect(x, 4096, PROT_READ | PROT_EXEC) == 0 && ((int (*)(void))x)() == 42;
        }
        CHECK(ok, "generated code runs");
    }

    /* 8: the main thread's stack size (JavaScriptCore asks for it) */
    {
        pthread_attr_t a;
        void *addr;
        size_t size = 0;
        int ok = pthread_getattr_np(pthread_self(), &a) == 0 && pthread_attr_getstack(&a, &addr, &size) == 0;
        printf("     main stack %zu KB\n", size >> 10);
        CHECK(ok && size >= (1 << 20), "main thread stack bounds");
    }

    /* 9: fork: the child's writes stay its own */
    {
        int *shared = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        pid_t pid;
        int st = -1;
        *shared = 1;
        pid = fork();
        if (pid == 0) {
            *shared = 2;
            _exit(*shared == 2 ? 0 : 1);
        }
        waitpid(pid, &st, 0);
        CHECK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 && *shared == 1, "fork copy-on-write of mmap memory");
    }

    if (failures) printf("vmtest: %d FAILED\n", failures);
    else printf("vmtest: ALL PASS\n");
    return failures ? 1 : 0;
}
