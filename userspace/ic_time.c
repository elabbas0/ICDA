/*
 * ic_time.c - TSC clock calibrated against the 100 Hz scheduler tick.
 *
 * Calibration waits for a tick edge, samples the TSC, waits two more
 * edges and samples again, so the measured window is exactly 20 ms of
 * tick time.  Emulators without an invariant TSC still get a usable
 * (if less exact) rate; if the TSC does not advance at all the clock
 * falls back to tick resolution.
 */
#include "ic_time.h"
#include "icda_sys.h"

#define IC_TICK_NS       10000000ULL  /* 100 Hz scheduler tick */
#define IC_CAL_TICKS     2ULL

static int      ic_time_ready;
static uint64_t ic_tsc_base;
static uint64_t ic_tick_base;
/* Nanoseconds per TSC cycle as 32.32 fixed point. */
static uint64_t ic_ns_per_cycle_q32;

static inline uint64_t ic_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Block until the next tick boundary.  Sleeping (not yield-spinning)
 * keeps the CPU free and wakes us right at the tick, so the TSC sample
 * taken after each wake sits at the same phase of the tick. */
static uint64_t ic_wait_tick_edge(void) {
    icda_sleep(1);
    return icda_ticks();
}

void ic_time_init(void) {
    uint64_t t0, t1, c0, c1;

    if (ic_time_ready) return;
    t0 = ic_wait_tick_edge();
    c0 = ic_rdtsc();
    icda_sleep(IC_CAL_TICKS);
    t1 = icda_ticks();
    c1 = ic_rdtsc();

    if (c1 > c0 && t1 > t0) {
        uint64_t ns = (t1 - t0) * IC_TICK_NS;
        ic_ns_per_cycle_q32 = (ns << 32) / (c1 - c0);
    } else {
        ic_ns_per_cycle_q32 = 0;
    }
    ic_tsc_base = c0;
    ic_tick_base = t0;
    ic_time_ready = 1;
}

uint64_t ic_time_ns(void) {
    uint64_t cycles;

    if (!ic_time_ready) ic_time_init();
    if (ic_ns_per_cycle_q32 == 0) {
        return (icda_ticks() - ic_tick_base) * IC_TICK_NS;
    }
    cycles = ic_rdtsc() - ic_tsc_base;
    /* 64x64->128 multiply: a slow emulated TSC makes the rate large
     * enough that a 64-bit product would overflow within seconds. */
    {
        __extension__ typedef unsigned __int128 u128;
        return (uint64_t)(((u128)cycles * ic_ns_per_cycle_q32) >> 32);
    }
}

uint64_t ic_time_us(void) {
    return ic_time_ns() / 1000ULL;
}

uint32_t ic_time_ms(void) {
    return (uint32_t)(ic_time_ns() / 1000000ULL);
}

float ic_time_s(void) {
    return (float)((double)ic_time_ns() * 1e-9);
}

/* ---- wall clock ---- */

static int ic_two(const char *p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

/* Monday-based weekday (Sakamoto's method). */
static int ic_weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int dow;
    if (m < 3) y -= 1;
    dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   /* 0 = Sunday */
    return (dow + 6) % 7;
}

int ic_wallclock(ic_datetime_t *out) {
    char buf[32];
    long n;
    int cc, yy;
    if (!out) return -1;
    n = (long)icda_read_file("/dev/rtc", buf, sizeof(buf) - 1);
    if (n < 19) return -1;
    cc = ic_two(buf);
    yy = ic_two(buf + 2);
    out->month = ic_two(buf + 5);
    out->day = ic_two(buf + 8);
    out->hour = ic_two(buf + 11);
    out->minute = ic_two(buf + 14);
    out->second = ic_two(buf + 17);
    if (cc < 0 || yy < 0 || out->month < 1 || out->month > 12 || out->day < 1 ||
        out->hour < 0 || out->minute < 0 || out->second < 0) {
        return -1;
    }
    out->year = cc * 100 + yy;
    out->weekday = ic_weekday(out->year, out->month, out->day);
    return 0;
}

static void ic_put(char *buf, int cap, int *pos, const char *s) {
    while (*s && *pos < cap - 1) buf[(*pos)++] = *s++;
    buf[*pos] = 0;
}

static void ic_put2(char *buf, int cap, int *pos, int v, int pad) {
    char t[3];
    int k = 0;
    if (v >= 10 || pad) t[k++] = (char)('0' + (v / 10) % 10);
    t[k++] = (char)('0' + v % 10);
    t[k] = 0;
    ic_put(buf, cap, pos, t);
}

void ic_format_hm(const ic_datetime_t *t, char *buf, int cap) {
    int pos = 0;
    if (!buf || cap <= 0) return;
    buf[0] = 0;
    if (!t) return;
    ic_put2(buf, cap, &pos, t->hour, 1);
    ic_put(buf, cap, &pos, ":");
    ic_put2(buf, cap, &pos, t->minute, 1);
}

void ic_format_day(const ic_datetime_t *t, char *buf, int cap) {
    static const char *const days[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int pos = 0;
    if (!buf || cap <= 0) return;
    buf[0] = 0;
    if (!t || t->weekday < 0 || t->weekday > 6 || t->month < 1 || t->month > 12) return;
    ic_put(buf, cap, &pos, days[t->weekday]);
    ic_put(buf, cap, &pos, " ");
    ic_put2(buf, cap, &pos, t->day, 0);
    ic_put(buf, cap, &pos, " ");
    ic_put(buf, cap, &pos, months[t->month - 1]);
}
