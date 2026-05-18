#include "speaker.h"

#include <stdint.h>

#define PIT_BASE_FREQUENCY 1193182U
#define PIT_COMMAND_PORT   0x43
#define PIT_CHANNEL2_PORT  0x42
#define SPEAKER_PORT       0x61

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
