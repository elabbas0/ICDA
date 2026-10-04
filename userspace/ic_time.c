








#include "ic_time.h"
#include "icda_sys.h"
#include "settings_store.h"

#define IC_TICK_NS       10000000ULL  
#define IC_CAL_TICKS     2ULL

static int      ic_time_ready;
static uint64_t ic_tsc_base;
static uint64_t ic_tick_base;

static uint64_t ic_ns_per_cycle_q32;

static inline uint64_t ic_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}




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



static int ic_two(const char *p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}


static int ic_weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int dow;
    if (m < 3) y -= 1;
    dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   
    return (dow + 6) % 7;
}

static int ic_days_in_month(int y, int m) {
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : days[m - 1];
}

static void ic_datetime_add_minutes(ic_datetime_t *t, int minutes) {
    int total = t->hour * 60 + t->minute + minutes;
    int day_shift = 0;
    while (total < 0) { total += 24 * 60; day_shift--; }
    while (total >= 24 * 60) { total -= 24 * 60; day_shift++; }
    t->hour = total / 60;
    t->minute = total % 60;
    while (day_shift > 0) {
        day_shift--;
        if (++t->day > ic_days_in_month(t->year, t->month)) {
            t->day = 1;
            if (++t->month > 12) { t->month = 1; t->year++; }
        }
    }
    while (day_shift < 0) {
        day_shift++;
        if (--t->day < 1) {
            if (--t->month < 1) { t->month = 12; t->year--; }
            t->day = ic_days_in_month(t->year, t->month);
        }
    }
}

static int ic_tz_cached = 0;
static uint32_t ic_tz_loaded_ms = 0;
static int ic_tz_loaded = 0;

void ic_time_reload_tz(void) {
    ic_tz_loaded = 0;
}

static int ic_tz_offset(void) {
    uint32_t now = ic_time_ms();
    if (!ic_tz_loaded || now - ic_tz_loaded_ms > 3000) {
        icda_settings_t s;
        icda_settings_load(&s);
        ic_tz_cached = s.tz_minutes;
        ic_tz_loaded_ms = now;
        ic_tz_loaded = 1;
    }
    return ic_tz_cached;
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
    ic_datetime_add_minutes(out, ic_tz_offset());
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
