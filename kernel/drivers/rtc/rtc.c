/*
 * rtc.c - CMOS real-time clock reader.
 *
 * The RTC updates its registers once a second; reading while an update
 * is in progress can mix old and new fields.  We wait for the
 * update-in-progress flag to clear, read everything, and repeat until
 * two consecutive reads agree.  Register B tells whether values are BCD
 * and whether the hour is 12-hour with a PM flag in bit 7.
 */
#include "rtc.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32   /* ACPI FADT default; 0 on many boards */
#define RTC_REG_A   0x0A
#define RTC_REG_B   0x0B

static inline void rtc_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t rtc_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint8_t cmos_read(uint8_t reg) {
    /* Bit 7 of the address port keeps NMIs disabled while selecting. */
    rtc_outb(CMOS_ADDR, (uint8_t)(0x80 | reg));
    return rtc_inb(CMOS_DATA);
}

static int rtc_updating(void) {
    return (cmos_read(RTC_REG_A) & 0x80) != 0;
}

typedef struct {
    uint8_t s, m, h, d, mo, y, c;
} rtc_raw_t;

static void rtc_read_raw(rtc_raw_t *r) {
    int guard = 100000;
    while (rtc_updating() && guard-- > 0) {
    }
    r->s = cmos_read(RTC_SECONDS);
    r->m = cmos_read(RTC_MINUTES);
    r->h = cmos_read(RTC_HOURS);
    r->d = cmos_read(RTC_DAY);
    r->mo = cmos_read(RTC_MONTH);
    r->y = cmos_read(RTC_YEAR);
    r->c = cmos_read(RTC_CENTURY);
}

static uint8_t bcd(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

int rtc_read(rtc_time_t *out) {
    rtc_raw_t a, b;
    uint8_t reg_b;
    int tries = 5;
    int pm;
    uint8_t century;

    if (!out) return -1;
    rtc_read_raw(&a);
    do {
        b = a;
        rtc_read_raw(&a);
    } while (--tries > 0 &&
             (a.s != b.s || a.m != b.m || a.h != b.h || a.d != b.d ||
              a.mo != b.mo || a.y != b.y));

    reg_b = cmos_read(RTC_REG_B);
    pm = (a.h & 0x80) != 0;
    a.h &= 0x7F;
    if (!(reg_b & 0x04)) {
        a.s = bcd(a.s);
        a.m = bcd(a.m);
        a.h = bcd(a.h);
        a.d = bcd(a.d);
        a.mo = bcd(a.mo);
        a.y = bcd(a.y);
        a.c = bcd(a.c);
    }
    if (!(reg_b & 0x02)) {
        /* 12-hour mode: 12 AM is 0, 12 PM is 12. */
        if (a.h == 12) a.h = 0;
        if (pm) a.h = (uint8_t)(a.h + 12);
    }
    if (a.mo < 1 || a.mo > 12 || a.d < 1 || a.d > 31 || a.h > 23 || a.m > 59 || a.s > 59) {
        return -1;
    }
    century = (a.c >= 19 && a.c <= 30) ? a.c : 20;
    out->year = (uint16_t)(century * 100 + a.y);
    out->month = a.mo;
    out->day = a.d;
    out->hour = a.h;
    out->minute = a.m;
    out->second = a.s;
    return 0;
}

static void put2(char *p, unsigned v) {
    p[0] = (char)('0' + (v / 10) % 10);
    p[1] = (char)('0' + v % 10);
}

uint64_t rtc_format(char *buf, uint64_t cap) {
    rtc_time_t t;
    if (!buf || cap < 21 || rtc_read(&t) != 0) return 0;
    put2(buf, t.year / 100u);
    put2(buf + 2, t.year % 100u);
    buf[4] = '-';
    put2(buf + 5, t.month);
    buf[7] = '-';
    put2(buf + 8, t.day);
    buf[10] = ' ';
    put2(buf + 11, t.hour);
    buf[13] = ':';
    put2(buf + 14, t.minute);
    buf[16] = ':';
    put2(buf + 17, t.second);
    buf[19] = '\n';
    buf[20] = '\0';
    return 20;
}
