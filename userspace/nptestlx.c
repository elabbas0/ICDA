












#include "icda_sys.h"

#define LX_READ      0
#define LX_WRITE     1
#define LX_OPEN      2
#define LX_CLOSE     3
#define LX_FSTAT     5
#define LX_LSEEK     8
#define LX_PREAD64   17
#define LX_MMAP      9
#define LX_MPROTECT  10
#define LX_MUNMAP    11
#define LX_BRK       12
#define LX_EXIT      60
#define LX_MREMAP    25
#define LX_FORK      57
#define LX_WAIT4     61
#define LX_MREMAP_MAYMOVE 1
#define LX_GETDENTS  217
#define LX_ARCH_PRCTL 158

#define LX_O_RDONLY 0
#define LX_O_CREAT  0100
#define LX_PROT_READ  1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC  4
#define LX_MAP_ANON 0x20
#define LX_MAP_PRIVATE 0x02
#define LX_ARCH_SET_FS 0x1002

#define U_EFAULT 14
#define U_EBADF  9
#define U_EINVAL 22
#define U_ENOENT 2
#define U_ENOMEM 12


typedef struct {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, _pad0;
    uint64_t st_rdev;
    int64_t  st_size, st_blksize, st_blocks;
    uint64_t st_times[6];
    int64_t  _reserved[3];
} lx_stat_t;

static int failures = 0;

static uint64_t lx_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void lx_puts(const char *s) {
    sys_call3(LX_WRITE, 1, (uint64_t)(uintptr_t)s, lx_strlen(s));
}

static int lx_streq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

/* the n-th (from 0) number in s[0 .. len), 0 if there are fewer */
static uint64_t nth_number(const char *s, long len, int n) {
    long i = 0;
    for (;;) {
        uint64_t v = 0;
        int digits = 0;
        while (i < len && (s[i] < '0' || s[i] > '9')) i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (uint64_t)(s[i] - '0');
            i++;
            digits = 1;
        }
        if (!digits) return 0;
        if (n-- == 0) return v;
    }
}

/* the number after key in s[0 .. len) ("MemFree:" in /proc/meminfo), 0 if absent */
static uint64_t number_after(const char *s, long len, const char *key) {
    long kl = (long)lx_strlen(key);
    for (long i = 0; i + kl <= len; i++) {
        long k = 0;
        while (k < kl && s[i + k] == key[k]) k++;
        if (k == kl) return nth_number(s + i + kl, len - i - kl, 0);
    }
    return 0;
}

/* 1 if key occurs in s[0 .. len) */
static int contains(const char *s, long len, const char *key) {
    long kl = (long)lx_strlen(key);
    for (long i = 0; i + kl <= len; i++) {
        long k = 0;
        while (k < kl && s[i + k] == key[k]) k++;
        if (k == kl) return 1;
    }
    return 0;
}

/* an anonymous mapping of len bytes with every page written (the memory in use), or 0 */
static uint64_t touch_new(uint64_t len) {
    uint64_t addr = sys_call6(LX_MMAP, 0, len, 3, LX_MAP_PRIVATE | LX_MAP_ANON, (uint64_t)(int64_t)-1, 0);
    if ((long)addr < 0) return 0;
    for (uint64_t i = 0; i < len; i += 4096) ((volatile char *)(uintptr_t)addr)[i] = 1;
    return addr;
}

static void check(const char *name, long rc, long want) {
    if (rc == want) {
        lx_puts("PASS ");
    } else {
        failures++;
        lx_puts("FAIL ");
    }
    lx_puts(name);
    lx_puts("\n");
}

/* the number after key in /proc/<file> (kB in meminfo and status), 0 if absent */
static uint64_t proc_number(const char *path, const char *key) {
    char b[2048];
    long fd = (long)sys_call3(LX_OPEN, (uint64_t)(uintptr_t)path, LX_O_RDONLY, 0), n;
    if (fd < 0) return 0;
    n = (long)sys_call3(LX_READ, (uint64_t)fd, (uint64_t)(uintptr_t)b, sizeof(b));
    sys_call1(LX_CLOSE, (uint64_t)fd);
    return n > 0 ? number_after(b, n, key) : 0;
}

/* what word w of the swap test's page i holds (gen: which writing) */
static uint64_t swap_word(uint64_t i, uint64_t w, uint64_t gen) {
    return (i * 0x9E3779B97F4A7C15ULL) ^ (w * 0xD1B54A32D192ED03ULL) ^ (gen * 0x5157A9ULL) ^ 0xA5A5A5A5ULL;
}

static void swap_fill(uint64_t addr, uint64_t first, uint64_t pages, uint64_t step, uint64_t gen) {
    for (uint64_t i = first; i < pages; i += step) {
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)(addr + i * 4096);
        for (uint64_t w = 0; w < 512; w++) p[w] = swap_word(i, w, gen);
    }
}

/* pages from first, every step, whose words are not gen's (the page index
 * in the pattern is base + i: a range moved by mremap keeps its words) */
static uint64_t swap_bad(uint64_t addr, uint64_t first, uint64_t pages, uint64_t step, uint64_t base, uint64_t gen) {
    uint64_t bad = 0;
    for (uint64_t i = first; i < pages; i += step) {
        const volatile uint64_t *p = (const volatile uint64_t *)(uintptr_t)(addr + i * 4096);
        for (uint64_t w = 0; w < 512; w++)
            if (p[w] != swap_word(base + i, w, gen)) {
                bad++;
                break;
            }
    }
    return bad;
}

static void lx_putnum(uint64_t n) {
    char b[24];
    int i = 23;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + n % 10);
        n /= 10;
    } while (n && i > 0);
    lx_puts(b + i);
}

/* the generation page j holds after the fill (1), the fork (2 on every 7th)
 * and the mprotect step (3 on pages 1, 8, 15 ... of the first quarter) */
static uint64_t swap_gen(uint64_t j, uint64_t quarter) {
    if (j % 7 == 0) return 2;
    if (j < quarter && j % 7 == 1) return 3;
    return 1;
}

/* every page of a range moved by mremap (page i was page base + i); the
 * first wrong one is described */
static uint64_t swap_bad_moved(uint64_t addr, uint64_t pages, uint64_t base, uint64_t quarter) {
    uint64_t bad = 0;
    for (uint64_t i = 0; i < pages; i++) {
        const volatile uint64_t *p = (const volatile uint64_t *)(uintptr_t)(addr + i * 4096);
        uint64_t gen = swap_gen(base + i, quarter);
        for (uint64_t w = 0; w < 512; w++)
            if (p[w] != swap_word(base + i, w, gen)) {
                if (!bad) {
                    uint64_t zero = 1, other = 0;
                    for (uint64_t k = 0; k < 512; k++) zero &= p[k] == 0;
                    for (uint64_t j = 0; j < pages + 2 * quarter && !other; j++)
                        if (p[0] == swap_word(j, 0, swap_gen(j, quarter))) other = j + 1;
                    lx_puts("  first wrong page ");
                    lx_putnum(i);
                    lx_puts(" of ");
                    lx_putnum(pages);
                    lx_puts(", word ");
                    lx_putnum(w);
                    lx_puts(zero ? ", all zero" : "");
                    lx_puts(", page ");
                    lx_putnum(base + i);
                    lx_puts(" gen ");
                    lx_putnum(gen);
                    if (other) {
                        lx_puts(", holds page ");
                        lx_putnum(other - 1);
                    }
                    lx_puts("\n");
                }
                bad++;
                break;
            }
    }
    if (bad) {
        lx_puts("  wrong pages: ");
        lx_putnum(bad);
        lx_puts("\n");
    }
    return bad;
}

/* Swap (kernel/memory/swap.c): more memory than the machine has, written
 * and read back word for word; then a fork, mprotect, mremap and munmap
 * over pages that are in swap.  Only with a swap area. */
static void swap_tests(void) {
    uint64_t total = proc_number("/proc/meminfo", "MemTotal:"), swap = proc_number("/proc/meminfo", "SwapTotal:");
    uint64_t len, pages, addr, free0, free1, cut;
    long pid;
    if (!swap) {
        lx_puts("SKIP swap: no swap area\n");
        return;
    }
    len = (total + total / 4) * 1024;
    if (len > (total + swap / 2) * 1024) len = (total + swap / 2) * 1024;
    len &= ~4095ULL;
    pages = len / 4096;
    free0 = proc_number("/proc/meminfo", "SwapFree:");
    addr = sys_call6(LX_MMAP, 0, len, 3, LX_MAP_PRIVATE | LX_MAP_ANON, (uint64_t)(int64_t)-1, 0);
    check("swap: map more than the memory", (long)addr > 0 ? 1 : (long)addr, 1);
    if ((long)addr <= 0) return;
    swap_fill(addr, 0, pages, 1, 1);
    free1 = proc_number("/proc/meminfo", "SwapFree:");
    check("swap: pages went to swap", free1 < free0 ? 1 : 0, 1);
    check("swap: status shows VmSwap", proc_number("/proc/self/status", "VmSwap:") > 0 ? 1 : 0, 1);
    check("swap: every word read back", (long)swap_bad(addr, 0, pages, 1, 0, 1), 0);

    /* a fork: the child reads the copy it had at the fork while the parent
     * writes over every 7th page */
    pid = (long)sys_call0(LX_FORK);
    if (pid == 0) {
        uint64_t bad = swap_bad(addr, 0, pages, 7, 0, 1);
        sys_call1(LX_EXIT, bad ? 1 : 0);
    }
    check("swap: fork", pid > 0 ? 1 : pid, 1);
    if (pid > 0) {
        int status = -1;
        swap_fill(addr, 0, pages, 7, 2);
        sys_call4(LX_WAIT4, (uint64_t)pid, (uint64_t)(uintptr_t)&status, 0, 0);
        check("swap: the child read its own copy back", status, 0);
        check("swap: the parent read its writes back", (long)swap_bad(addr, 0, pages, 7, 0, 2), 0);
        check("swap: and the pages it did not write", (long)swap_bad(addr, 3, pages, 7, 0, 1), 0);
    }

    /* read-only while in swap, then writable again */
    cut = (pages / 4) * 4096;
    check("swap: mprotect read-only", (long)sys_call3(LX_MPROTECT, addr, cut, LX_PROT_READ), 0);
    check("swap: read back read-only", (long)swap_bad(addr, 1, cut / 4096, 7, 0, 1), 0);
    check("swap: mprotect read-write", (long)sys_call3(LX_MPROTECT, addr, cut, LX_PROT_READ | LX_PROT_WRITE), 0);
    swap_fill(addr, 1, cut / 4096, 7, 3);
    check("swap: written after", (long)swap_bad(addr, 1, cut / 4096, 7, 0, 3), 0);

    /* mremap moves pages that are in swap with the rest */
    {
        uint64_t from = addr + cut, size = (pages / 4) * 4096, to;
        to = sys_call5(LX_MREMAP, from, size, size + 4096, LX_MREMAP_MAYMOVE, 0);
        check("swap: mremap", (long)to > 0 ? 1 : (long)to, 1);
        if ((long)to > 0) {
            check("swap: moved pages read back", (long)swap_bad_moved(to, size / 4096, cut / 4096, cut / 4096), 0);
            sys_call2(LX_MUNMAP, to, size + 4096);
        }
    }

    sys_call2(LX_MUNMAP, addr, len);
    check("swap: munmap gives the slots back", proc_number("/proc/meminfo", "SwapFree:") + 256 >= free0 ? 1 : 0, 1);
}

int nptestlx_main(int argc, char **argv) {
    char buf[2048];
    char buf2[1024];
    lx_stat_t st;
    long fd;
    long n;

    



    if (argc > 1 && lx_streq(argv[1], "suicide")) {
        uint64_t addr = sys_call6(LX_MMAP, 0, 8192, 3,
                                  LX_MAP_PRIVATE | LX_MAP_ANON,
                                  (uint64_t)(int64_t)-1, 0);
        volatile char *p;
        if ((long)addr < 0) {
            sys_call1(LX_EXIT, 43);
        }
        if ((long)sys_call3(LX_MPROTECT, addr, 8192, LX_PROT_READ) != 0) {
            sys_call1(LX_EXIT, 44);
        }
        p = (volatile char *)(uintptr_t)addr;
        p[0] = 'x';
        sys_call1(LX_EXIT, 42);
    }

    lx_puts("nptestlx: linux-personality syscall tests\n");

    n = (long)sys_call3(LX_WRITE, 1, (uint64_t)(uintptr_t)"t", 1);
    check("write stdout", n, 1);

    fd = (long)sys_call3(LX_OPEN, (uint64_t)(uintptr_t)"/etc/motd.txt",
                         LX_O_RDONLY, 0);
    check("open motd >= 3", fd >= 3 ? 1 : fd, 1);
    if (fd >= 3) {
        n = (long)sys_call3(LX_READ, (uint64_t)fd,
                            (uint64_t)(uintptr_t)buf, sizeof(buf));
        check("read motd > 0", n > 0 ? 1 : n, 1);
        n = (long)sys_call3(LX_FSTAT, (uint64_t)fd,
                            (uint64_t)(uintptr_t)&st, 0);
        check("fstat ok", n, 0);
        check("fstat size > 0", st.st_size > 0 ? 1 : 0, 1);
        check("fstat type file", (st.st_mode & 0170000) == 0100000 ? 1 : 0, 1);
        check("close ok", (long)sys_call1(LX_CLOSE, (uint64_t)fd), 0);
        
        check("close twice -> EBADF",
              (long)sys_call1(LX_CLOSE, (uint64_t)fd), -(long)U_EBADF);
    }

    check("open missing -> ENOENT",
          (long)sys_call3(LX_OPEN, (uint64_t)(uintptr_t)"/no/such/file",
                          LX_O_RDONLY, 0),
          -(long)U_ENOENT);
    check("read bad fd -> EBADF",
          (long)sys_call3(LX_READ, 99, (uint64_t)(uintptr_t)buf, 64),
          -(long)U_EBADF);
    check("write bad fd -> EBADF",
          (long)sys_call3(LX_WRITE, 99, (uint64_t)(uintptr_t)"x", 1),
          -(long)U_EBADF);

    
    fd = (long)sys_call3(LX_OPEN, (uint64_t)(uintptr_t)"/",
                         LX_O_RDONLY, 0);
    check("open dir >= 3", fd >= 3 ? 1 : fd, 1);
    if (fd >= 3) {
        long n1 = (long)sys_call3(LX_GETDENTS, (uint64_t)fd,
                                  (uint64_t)(uintptr_t)buf, sizeof(buf));
        long n2;
        check("getdents > 0", n1 > 0 ? 1 : n1, 1);
        n2 = (long)sys_call3(LX_GETDENTS, (uint64_t)fd,
                             (uint64_t)(uintptr_t)buf2, sizeof(buf2));
        check("getdents second call ok", n2 >= 0 ? 0 : n2, 0);
        check("close dir", (long)sys_call1(LX_CLOSE, (uint64_t)fd), 0);
        check("getdents closed -> EBADF",
              (long)sys_call3(LX_GETDENTS, (uint64_t)fd,
                              (uint64_t)(uintptr_t)buf, 64),
              -(long)U_EBADF);
    }
    check("getdents stdio -> ENOTDIR",
          (long)sys_call3(LX_GETDENTS, 1, (uint64_t)(uintptr_t)buf, 64),
          -20L);

    
    {
        uint64_t b0 = sys_call1(LX_BRK, 0);
        uint64_t b1;
        volatile char *pb;
        check("brk(0) nonzero", b0 != 0 ? 1 : 0, 1);
        b1 = sys_call1(LX_BRK, b0 + 8192);
        check("brk grow", b1 == b0 + 8192 ? 1 : 0, 1);
        pb = (volatile char *)(uintptr_t)(b0 + 4096);
        pb[0] = 'B';
        check("brk page touchback", pb[0] == 'B' ? 1 : 0, 1);
    }

    
    {
        uint64_t addr = sys_call6(LX_MMAP, 0, 8192, 3,
                                  LX_MAP_PRIVATE | LX_MAP_ANON,
                                  (uint64_t)(int64_t)-1, 0);
        volatile char *p;
        uint64_t i;
        check("mmap anon ok", (long)addr >= 0x70000000 ? 1 : (long)addr, 1);
        if ((long)addr >= 0) {
            p = (volatile char *)(uintptr_t)addr;
            for (i = 0; i < 8192; i++) p[i] = (char)(i & 0x7F);
            check("mmap touchback", p[1234] == (char)(1234 & 0x7F) ? 1 : 0, 1);
            check("mprotect RO",
                  (long)sys_call3(LX_MPROTECT, addr, 8192, LX_PROT_READ), 0);
            check("mprotect RW back",
                  (long)sys_call3(LX_MPROTECT, addr, 8192,
                                  LX_PROT_READ | LX_PROT_WRITE),
                  0);
            check("mprotect W|X -> EINVAL",
                  (long)sys_call3(LX_MPROTECT, addr, 8192,
                                  LX_PROT_WRITE | LX_PROT_EXEC),
                  -(long)U_EINVAL);
            check("mprotect unmapped -> ENOMEM",
                  (long)sys_call3(LX_MPROTECT, 0x1000, 4096, LX_PROT_READ),
                  -(long)U_ENOMEM);
            check("munmap ok",
                  (long)sys_call2(LX_MUNMAP, addr, 8192), 0);
        }
        check("munmap unaligned -> EINVAL",
              (long)sys_call2(LX_MUNMAP, 0x70000001, 4096),
              -(long)U_EINVAL);
        check("munmap FB window -> EINVAL",
              (long)sys_call2(LX_MUNMAP, 0x500000000ULL, 4096),
              -(long)U_EINVAL);
        check("munmap kernel half -> EINVAL",
              (long)sys_call2(LX_MUNMAP, 0xFFFFFFFF80000000ULL, 4096),
              -(long)U_EINVAL);
    }

    

    check("arch_prctl stub ok",
          (long)sys_call2(LX_ARCH_PRCTL, LX_ARCH_SET_FS, 0x70000000), 0);

    /* /proc as WebKit reads it.  Its allocator opens /proc/self/statm once
     * and preads it again at offset 0 for its footprint; its memory monitor
     * keeps /proc/meminfo open and rewinds it.  Both must see memory change. */
    {
        long pfd = (long)sys_call3(LX_OPEN, (uint64_t)(uintptr_t)"/proc/self/statm", LX_O_RDONLY, 0);
        check("open /proc/self/statm", pfd >= 3 ? 1 : pfd, 1);
        if (pfd >= 3) {
            uint64_t r0, r1, addr;
            n = (long)sys_call4(LX_PREAD64, (uint64_t)pfd, (uint64_t)(uintptr_t)buf, sizeof(buf), 0);
            check("pread /proc/self/statm", n > 0 ? 1 : n, 1);
            r0 = n > 0 ? nth_number(buf, n, 1) : 0;
            check("statm resident > 0", r0 > 0 ? 1 : 0, 1);
            addr = touch_new(4ULL << 20);
            check("statm: 4 MB touched", addr ? 1 : 0, 1);
            n = (long)sys_call4(LX_PREAD64, (uint64_t)pfd, (uint64_t)(uintptr_t)buf, sizeof(buf), 0);
            r1 = n > 0 ? nth_number(buf, n, 1) : 0;
            check("statm resident follows memory use (pread at 0)", r1 >= r0 + 1024 ? 1 : 0, 1);
            if (addr) sys_call2(LX_MUNMAP, addr, 4ULL << 20);
            check("close statm", (long)sys_call1(LX_CLOSE, (uint64_t)pfd), 0);
        }
    }
    {
        long mfd = (long)sys_call3(LX_OPEN, (uint64_t)(uintptr_t)"/proc/meminfo", LX_O_RDONLY, 0);
        check("open /proc/meminfo", mfd >= 3 ? 1 : mfd, 1);
        if (mfd >= 3) {
            uint64_t f0, f1, a0, addr;
            n = (long)sys_call3(LX_READ, (uint64_t)mfd, (uint64_t)(uintptr_t)buf, sizeof(buf));
            f0 = n > 0 ? number_after(buf, n, "MemFree:") : 0;
            a0 = n > 0 ? number_after(buf, n, "MemAvailable:") : 0;
            check("meminfo MemFree > 0", f0 > 0 ? 1 : 0, 1);
            check("meminfo MemAvailable >= MemFree", a0 >= f0 ? 1 : 0, 1);
            addr = touch_new(16ULL << 20);
            check("meminfo: 16 MB touched", addr ? 1 : 0, 1);
            check("lseek /proc/meminfo to 0", (long)sys_call3(LX_LSEEK, (uint64_t)mfd, 0, 0), 0);
            n = (long)sys_call3(LX_READ, (uint64_t)mfd, (uint64_t)(uintptr_t)buf, sizeof(buf));
            f1 = n > 0 ? number_after(buf, n, "MemFree:") : 0;
            check("meminfo follows memory use after a rewind", f1 && f1 + 8192 <= f0 ? 1 : 0, 1);
            if (addr) sys_call2(LX_MUNMAP, addr, 16ULL << 20);
            check("close meminfo", (long)sys_call1(LX_CLOSE, (uint64_t)mfd), 0);
        }
    }
    {
        long sfd = (long)sys_call3(LX_OPEN, (uint64_t)(uintptr_t)"/proc/self/status", LX_O_RDONLY, 0);
        check("open /proc/self/status", sfd >= 3 ? 1 : sfd, 1);
        if (sfd >= 3) {
            n = (long)sys_call3(LX_READ, (uint64_t)sfd, (uint64_t)(uintptr_t)buf, sizeof(buf));
            check("status has Threads and RssAnon", n > 0 && contains(buf, n, "Threads:") && contains(buf, n, "RssAnon:") ? 1 : 0, 1);
            sys_call1(LX_CLOSE, (uint64_t)sfd);
        }
        sfd = (long)sys_call3(LX_OPEN, (uint64_t)(uintptr_t)"/proc/self", LX_O_RDONLY, 0);
        if (sfd >= 3) {
            n = (long)sys_call3(LX_GETDENTS, (uint64_t)sfd, (uint64_t)(uintptr_t)buf, sizeof(buf));
            check("/proc/self lists statm", n > 0 && contains(buf, n, "statm") ? 1 : 0, 1);
            sys_call1(LX_CLOSE, (uint64_t)sfd);
        }
    }

    swap_tests();

    if (failures == 0) {
        lx_puts("nptestlx: ALL PASS\n");
    } else {
        lx_puts("nptestlx: FAILURES PRESENT\n");
    }
    return failures;
}
