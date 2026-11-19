












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

    if (failures == 0) {
        lx_puts("nptestlx: ALL PASS\n");
    } else {
        lx_puts("nptestlx: FAILURES PRESENT\n");
    }
    return failures;
}
