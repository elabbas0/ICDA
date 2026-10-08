/* The vDSO of ICDA's Linux personality: clock_gettime without a system call.
 *
 * Linux programs ask for the time constantly (WebKit's run loops, timers and
 * animations: thousands of times a second), and each system call is a trip
 * through the kernel and its lock.  The kernel maps this small library and a
 * read-only data page (vvar, right below it) into every Linux process and
 * names it in the auxiliary vector (AT_SYSINFO_EHDR); musl finds
 * __vdso_clock_gettime there and calls it instead.
 *
 * The clock is the kernel's own, computed the same way (lx.c): monotonic
 * time is whole microseconds of the TSC, real time adds the boot time.  So
 * the two never disagree.  Built freestanding: no libc, no relocations. */
#include <stdint.h>

struct vvar {
    uint64_t per_us;          /* TSC counts per microsecond; 0: not ready, use the system call */
    uint64_t real_offset_ns;  /* real time minus monotonic time */
};

extern const volatile struct vvar __vvar __attribute__((visibility("hidden")));

struct ts { int64_t sec, nsec; };

static long sys_clock_gettime(long clk, struct ts *ts) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(228L), "D"(clk), "S"(ts) : "rcx", "r11", "memory");
    return r;
}

int __vdso_clock_gettime(long clk, struct ts *ts) {
    uint64_t per_us = __vvar.per_us, ns;
    uint32_t lo, hi;
    if (!per_us || clk < 0 || clk > 11) return (int)sys_clock_gettime(clk, ts);
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    ns = ((((uint64_t)hi << 32) | lo) / per_us) * 1000ULL;
    if (clk == 0 || clk == 5 || clk == 8 || clk == 11) ns += __vvar.real_offset_ns;
    ts->sec = (int64_t)(ns / 1000000000ULL);
    ts->nsec = (int64_t)(ns % 1000000000ULL);
    return 0;
}

int clock_gettime(long, struct ts *) __attribute__((weak, alias("__vdso_clock_gettime")));
