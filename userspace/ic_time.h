/*
 * ic_time.h - monotonic high-resolution clock for userspace.
 *
 * The kernel exposes only a 100 Hz tick (SYS_TICKS) and the native ABI
 * is frozen, so fine-grained time is derived in userspace from the TSC,
 * calibrated once against the tick.  Motion code (ic_anim.h) runs on
 * this clock so animation speed is independent of frame rate and load.
 *
 * rdtsc is unprivileged (the kernel never sets CR4.TSD).
 */
#ifndef USERSPACE_IC_TIME_H
#define USERSPACE_IC_TIME_H

#include <stdint.h>

/* Calibrate the TSC against the scheduler tick.  Blocks for about two
 * ticks (~20 ms) the first time; later calls return immediately.  Every
 * other ic_time_* call calibrates lazily, so calling this explicitly is
 * only useful to move the cost to a convenient moment (app start). */
void ic_time_init(void);

/* Monotonic time since the first ic_time_* call. */
uint64_t ic_time_ns(void);
uint64_t ic_time_us(void);
uint32_t ic_time_ms(void);

/* Seconds as float, for animation math. */
float ic_time_s(void);

/* ---- wall clock (read from /dev/rtc) ---- */

typedef struct {
    int year, month, day;       /* month 1..12 */
    int hour, minute, second;
    int weekday;                /* 0 = Monday .. 6 = Sunday */
} ic_datetime_t;

/* Returns 0 on success, -1 when no clock is available. */
int  ic_wallclock(ic_datetime_t *out);
/* "14:05" */
void ic_format_hm(const ic_datetime_t *t, char *buf, int cap);
/* "Mon 28 Sep" */
void ic_format_day(const ic_datetime_t *t, char *buf, int cap);

#endif /* USERSPACE_IC_TIME_H */
