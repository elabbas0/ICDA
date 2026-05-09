#include "serial.h"
#include <stdint.h>

#define COM1 0x3F8

static int initialized = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);    // Disable interrupts
    outb(COM1 + 3, 0x80);    // Enable DLAB
    outb(COM1 + 0, 0x03);    // Divisor low: 38400 baud
    outb(COM1 + 1, 0x00);    // Divisor high
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);    // Enable FIFO, clear it, 14-byte threshold
    outb(COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
    initialized = 1;
}

int serial_ready(void) {
    return initialized;
}

void serial_write_char(char c) {
    if (!initialized) return;

    if (c == '\n') {
        serial_write_char('\r');
    }

    while ((inb(COM1 + 5) & 0x20) == 0) {
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *str) {
    if (!initialized || !str) return;

    for (int i = 0; str[i] != '\0'; i++) {
        serial_write_char(str[i]);
    }
}
