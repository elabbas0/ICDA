/*
 * net_drv.c — Network driver abstraction layer.
 *
 * Probes for virtio-net first (paravirtual, fast on VirtualBox/QEMU),
 * then falls back to e1000 (emulated, works everywhere).
 */
#include "net_drv.h"
#include "e1000.h"
#include "virtio_net.h"
#include "../serial/serial.h"

typedef enum {
    NET_DRV_NONE = 0,
    NET_DRV_VIRTIO,
    NET_DRV_E1000,
} net_drv_type_t;

static net_drv_type_t active_driver = NET_DRV_NONE;

int net_drv_init(void) {
    /* Try e1000 first (works in QEMU and VirtualBox) */
    if (e1000_init() == 0) {
        active_driver = NET_DRV_E1000;
        serial_write("[net-drv] e1000 initialized\n");
        return 0;
    }

    active_driver = NET_DRV_NONE;
    serial_write("[net-drv] no network adapter found\n");
    return -1;
}

int net_drv_send_frame(const void *data, uint16_t len) {
    switch (active_driver) {
        case NET_DRV_VIRTIO:  return virtio_net_send_frame(data, len);
        case NET_DRV_E1000:   return e1000_send_frame(data, len);
        default: return -1;
    }
}

int net_drv_recv_frame(void *data, uint16_t cap, uint16_t *len_out) {
    switch (active_driver) {
        case NET_DRV_VIRTIO:  return virtio_net_recv_frame(data, cap, len_out);
        case NET_DRV_E1000:   return e1000_recv_frame(data, cap, len_out);
        default: return -1;
    }
}

int net_drv_mac(uint8_t out[6]) {
    switch (active_driver) {
        case NET_DRV_VIRTIO:  return virtio_net_mac(out);
        case NET_DRV_E1000:   return e1000_mac(out);
        default: return -1;
    }
}

int net_drv_ready(void) {
    switch (active_driver) {
        case NET_DRV_VIRTIO:  return virtio_net_ready();
        case NET_DRV_E1000:   return e1000_ready();
        default: return 0;
    }
}
