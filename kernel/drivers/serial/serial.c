#include "serial.h"
#include "../../fs/bootlog.h"
#include "../device.h"
#include <stdint.h>

#define COM1 0x3F8

static int initialized = 0;
static kernel_device_t serial_device;

static void serial_device_write(void *context, const char *str) {
    (void)context;
    serial_write(str);
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);    
    outb(COM1 + 3, 0x80);    
    outb(COM1 + 0, 0x03);    
    outb(COM1 + 1, 0x00);    
    outb(COM1 + 3, 0x03);    
    outb(COM1 + 2, 0xC7);    
    outb(COM1 + 4, 0x0B);    
    initialized = 1;

    serial_device.name = "serial";
    serial_device.class_id = DEVICE_CLASS_SERIAL;
    static const serial_device_ops_t ops = {
        .write = serial_device_write
    };
    serial_device.ops = &ops;
    serial_device.context = 0;
    serial_device.next = 0;
    device_register(&serial_device);
}

int serial_ready(void) {
    return initialized;
}

void serial_write_char(char c) {
    bootlog_putc(c);   /* also kept for /BOOTLOG.TXT: laptops have no COM1 */
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
