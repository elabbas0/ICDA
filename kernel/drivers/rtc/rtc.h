#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* CMOS real-time clock (MC146818-compatible, ports 0x70/0x71).
 * Values are whatever the firmware keeps in CMOS - UTC on QEMU's
 * default and most Linux-installed machines. */
typedef struct {
    uint16_t year;
    uint8_t  month;   /* 1..12 */
    uint8_t  day;     /* 1..31 */
    uint8_t  hour;    /* 0..23 */
    uint8_t  minute;
    uint8_t  second;
} rtc_time_t;

/* Read a consistent snapshot (retries across update cycles).
 * Returns 0 on success, -1 if the clock looks absent. */
int rtc_read(rtc_time_t *out);

/* Format "YYYY-MM-DD HH:MM:SS\n" into buf; returns bytes written
 * (excluding the NUL) or 0 on failure. */
uint64_t rtc_format(char *buf, uint64_t cap);

#endif
