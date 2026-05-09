#include "device.h"

static kernel_device_t *device_list = 0;

static int streq(const char *a, const char *b) {
    int i = 0;
    if (!a || !b) {
        return 0;
    }

    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }

    return a[i] == '\0' && b[i] == '\0';
}

void device_register(kernel_device_t *device) {
    if (!device) {
        return;
    }

    device->next = device_list;
    device_list = device;
}

kernel_device_t *device_first(device_class_t class_id) {
    kernel_device_t *device = device_list;

    while (device) {
        if (device->class_id == class_id) {
            return device;
        }
        device = device->next;
    }

    return 0;
}

kernel_device_t *device_find(const char *name) {
    kernel_device_t *device = device_list;

    while (device) {
        if (streq(device->name, name)) {
            return device;
        }
        device = device->next;
    }

    return 0;
}
