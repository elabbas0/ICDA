#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

typedef enum {
    DEVICE_CLASS_DISPLAY = 0,
    DEVICE_CLASS_SERIAL,
    DEVICE_CLASS_INPUT
} device_class_t;

typedef struct {
    void (*clear)(void *context);
    void (*write)(void *context, const char *str, uint32_t fg, uint32_t bg);
    void (*backspace)(void *context, uint32_t bg);
} display_device_ops_t;

typedef struct {
    void (*write)(void *context, const char *str);
} serial_device_ops_t;

typedef struct {
    int (*has_char)(void *context);
    int (*read_char)(void *context);
} input_device_ops_t;

typedef struct kernel_device {
    const char *name;
    device_class_t class_id;
    const void *ops;
    void *context;
    struct kernel_device *next;
} kernel_device_t;

void device_register(kernel_device_t *device);
kernel_device_t *device_first(device_class_t class_id);
kernel_device_t *device_find(const char *name);

#endif
