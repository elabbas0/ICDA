#include "tsc.h"

/* Frequency sources, best first:
 *   CPUID 0x15  TSC/crystal ratio (crystal 24 MHz on Skylake/Kaby Lake
 *               client parts, which report it as 0)
 *   CPUID 0x16  nominal base frequency in MHz
 *   PIT channel 2, polled through port 0x61 (no interrupt needed)
 *   2 GHz guess, so delays still end */

static uint64_t hz = 2000000000ULL;
static uint64_t per_us = 2000;
static const char *source = "default 2 GHz";

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

uint64_t tsc_read(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static int plausible(uint64_t f) {
    return f >= 100000000ULL && f <= 10000000000ULL;
}

static uint64_t from_cpuid(void) {
    uint32_t a, b, c, d, max, family, model;
    cpuid(0, &max, &b, &c, &d);
    cpuid(1, &a, &b, &c, &d);
    family = (a >> 8) & 0xF;
    model = ((a >> 4) & 0xF) | (((a >> 16) & 0xF) << 4);
    if (max >= 0x15) {
        cpuid(0x15, &a, &b, &c, &d);
        if (a && b) {
            uint64_t crystal = c;
            if (!crystal && family == 6 &&
                (model == 0x4E || model == 0x5E || model == 0x8E || model == 0x9E))
                crystal = 24000000ULL;
            if (crystal && plausible(crystal * b / a)) {
                source = "cpuid 0x15";
                return crystal * b / a;
            }
        }
    }
    if (max >= 0x16) {
        cpuid(0x16, &a, &b, &c, &d);
        if (plausible((uint64_t)(a & 0xFFFF) * 1000000ULL)) {
            source = "cpuid 0x16";
            return (uint64_t)(a & 0xFFFF) * 1000000ULL;
        }
    }
    return 0;
}

/* 10 ms one-shot on PIT channel 2; OUT2 is readable in port 0x61 bit 5. */
static uint64_t from_pit(void) {
    uint8_t saved = inb(0x61);
    uint64_t t0, t1;
    outb(0x61, (uint8_t)((saved & ~0x02) | 0x01));   /* gate on, speaker off */
    outb(0x43, 0xB0);                                /* ch2, lo/hi, mode 0 */
    outb(0x42, 11932 & 0xFF);
    outb(0x42, 11932 >> 8);
    t0 = tsc_read();
    while (!(inb(0x61) & 0x20)) {
        if (tsc_read() - t0 > 20000000000ULL) {      /* PIT not counting */
            outb(0x61, saved);
            return 0;
        }
    }
    t1 = tsc_read();
    outb(0x61, saved);
    if (!plausible((t1 - t0) * 100)) return 0;
    source = "pit channel 2";
    return (t1 - t0) * 100;
}

void tsc_init(void) {
    uint64_t f = from_cpuid();
    if (!f) f = from_pit();
    if (f) hz = f;
    per_us = hz / 1000000ULL;
    if (!per_us) per_us = 1;
}

uint64_t tsc_hz(void) { return hz; }
const char *tsc_source(void) { return source; }

uint64_t tsc_us(void) {
    return tsc_read() / per_us;
}

void udelay(uint64_t us) {
    uint64_t start = tsc_read(), cycles = us * per_us;
    while (tsc_read() - start < cycles) __asm__ volatile("pause");
}
