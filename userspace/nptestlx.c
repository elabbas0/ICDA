/* Linux-personality syscall test (runs as /bin/nptestlx.elf).
 *
 * Exercises linux_syscall_dispatch through real Linux syscall numbers:
 * open/read/write/close/fstat/getdents/brk/mmap/munmap/mprotect and the
 * arch_prctl stub. Suites:
 *   - normal mode: behavioural + negative tests, exit code = failures
 *   - "suicide" argv mode: mprotect a page read-only, then write it;
 *     the kernel must kill us (-11). Survival exits 42.
 * The native nptest.app spawns the suicide mode and checks the -11.
 *
 * Run headless via the boot self-test hook:
 *   kernel cmdline += icda.test=nptestlx
 */
#include "icda_sys.h"

#define LX_READ      0
#define LX_WRITE     1
#define LX_OPEN      2
#define LX_CLOSE     3
#define LX_FSTAT     5
#define LX_MMAP      9
#define LX_MPROTECT  10
#define LX_MUNMAP    11
#define LX_BRK       12
#define LX_EXIT      60
#define LX_GETDENTS  78
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

/* Kernel vfs_stat_t layout (see kernel/fs/vfs.h). */
typedef struct {
    uint64_t st_inode;
    uint64_t st_size;
    uint64_t st_created;
    uint64_t st_modified;
    uint8_t  st_type;
    uint8_t  st_readonly;
    uint8_t  _pad[6];
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

    /* Suicide mode: used by native nptest to verify the W^X kill path.
     * mprotect a fresh mapping read-only, then store through it. The
     * kernel must terminate us with -11. Reaching the exit below with
     * 42 means protection did NOT fire. */
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
        check("fstat type file", st.st_type, 1);
        check("close ok", (long)sys_call1(LX_CLOSE, (uint64_t)fd), 0);
        /* Double close must fail, not silently succeed. */
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
    check("write stdin -> EBADF",
          (long)sys_call3(LX_WRITE, 0, (uint64_t)(uintptr_t)"x", 1),
          -(long)U_EBADF);
    check("read stdout -> EBADF",
          (long)sys_call3(LX_READ, 1, (uint64_t)(uintptr_t)buf, 64),
          -(long)U_EBADF);

    /* getdents pages through the fd offset. */
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
    check("getdents stdio -> EBADF",
          (long)sys_call3(LX_GETDENTS, 1, (uint64_t)(uintptr_t)buf, 64),
          -(long)U_EBADF);

    /* brk ladder. */
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

    /* mmap / mprotect / munmap incl. W^X and guard windows. */
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

    /* arch_prctl stub documents itself: returns 0, programs nothing
     * (real FSBASE is a later compat-ladder rung). */
    check("arch_prctl stub ok",
          (long)sys_call2(LX_ARCH_PRCTL, LX_ARCH_SET_FS, 0x70000000), 0);

    if (failures == 0) {
        lx_puts("nptestlx: ALL PASS\n");
    } else {
        lx_puts("nptestlx: FAILURES PRESENT\n");
    }
    return failures;
}
