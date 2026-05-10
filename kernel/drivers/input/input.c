#include "input.h"

#include "../device.h"

static kernel_device_t *cached_input_device = 0;

static kernel_device_t *input_device(void) {
    if (!cached_input_device) {
        cached_input_device = device_first(DEVICE_CLASS_INPUT);
    }
    return cached_input_device;
}

int input_has_char(void) {
    kernel_device_t *device = input_device();
    const input_device_ops_t *ops;

    if (!device) {
        return 0;
    }

    ops = (const input_device_ops_t *)device->ops;
    return ops->has_char(device->context);
}

int input_read_char(void) {
    kernel_device_t *device = input_device();
    const input_device_ops_t *ops;

    if (!device) {
        return -1;
    }

    ops = (const input_device_ops_t *)device->ops;
    return ops->read_char(device->context);
}
