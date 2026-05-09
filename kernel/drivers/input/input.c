#include "input.h"

#include "../device.h"

int input_has_char(void) {
    kernel_device_t *device = device_first(DEVICE_CLASS_INPUT);
    const input_device_ops_t *ops;

    if (!device) {
        return 0;
    }

    ops = (const input_device_ops_t *)device->ops;
    return ops->has_char(device->context);
}

int input_read_char(void) {
    kernel_device_t *device = device_first(DEVICE_CLASS_INPUT);
    const input_device_ops_t *ops;

    if (!device) {
        return -1;
    }

    ops = (const input_device_ops_t *)device->ops;
    return ops->read_char(device->context);
}
