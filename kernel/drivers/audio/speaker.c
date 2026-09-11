#include "speaker.h"

#include <stdint.h>

#define PIT_BASE_FREQUENCY 1193182U
#define PIT_COMMAND_PORT   0x43
#define PIT_CHANNEL2_PORT  0x42
#define SPEAKER_PORT       0x61

/* Slice C audio hardening: the tone duration comes straight from a
 * userspace syscall argument, so it must be capped - an unbounded
 * busy-spin here would hang the calling thread (and, on real HW,
 * needlessly blast the speaker). 500 ticks = 5 s at 100 Hz. */
#define SPEAKER_MAX_TICKS  500U
#define SPEAKER_MIN_HZ     30U
#define SPEAKER_MAX_HZ     8000U

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void speaker_init(void) {
    speaker_stop();
}

void speaker_stop(void) {
    uint8_t state = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, (uint8_t)(state & ~0x03U));
}

void speaker_play(uint32_t frequency_hz) {
    uint32_t divisor;
    uint8_t state;

    if (frequency_hz == 0) {
        speaker_stop();
        return;
    }
    /* Clamp out-of-range requests instead of programming garbage
     * divisors into the PIT. */
    if (frequency_hz < SPEAKER_MIN_HZ) {
        frequency_hz = SPEAKER_MIN_HZ;
    }
    if (frequency_hz > SPEAKER_MAX_HZ) {
        frequency_hz = SPEAKER_MAX_HZ;
    }

    divisor = PIT_BASE_FREQUENCY / frequency_hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFFU) {
        divisor = 0xFFFFU;
    }

    outb(PIT_COMMAND_PORT, 0xB6);
    outb(PIT_CHANNEL2_PORT, (uint8_t)(divisor & 0xFFU));
    outb(PIT_CHANNEL2_PORT, (uint8_t)((divisor >> 8) & 0xFFU));

    state = inb(SPEAKER_PORT);
    if ((state & 0x03U) != 0x03U) {
        outb(SPEAKER_PORT, (uint8_t)(state | 0x03U));
    }
}

void speaker_play_for(uint32_t frequency_hz, uint64_t ticks) {
    volatile uint64_t outer;
    volatile uint64_t inner;
    uint64_t delay_units;

    if (ticks == 0) {
        if (frequency_hz == 0) {
            speaker_stop();
        } else {
            speaker_play(frequency_hz);
        }
        return;
    }

    /* Bounded busy-wait only: cap the duration so a bad/huge argument
     * can never spin the CPU (or the speaker) indefinitely. */
    if (ticks > SPEAKER_MAX_TICKS) {
        ticks = SPEAKER_MAX_TICKS;
    }
    speaker_play(frequency_hz);
    delay_units = ticks * 200000ULL;
    if (delay_units < 40000ULL) {
        delay_units = 40000ULL;
    }
    for (outer = 0; outer < delay_units; outer++) {
        inner = outer;
        __asm__ volatile("" : "+r"(inner));
    }
    speaker_stop();
}
