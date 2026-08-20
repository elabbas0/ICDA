#ifndef NET_DRV_H
#define NET_DRV_H

#include <stdint.h>

/*
 * Network driver abstraction layer.
 * The net stack calls these instead of e1000 directly.
 * On init, the kernel probes for virtio-net first, then e1000.
 */

int net_drv_init(void);
int net_drv_send_frame(const void *data, uint16_t len);
int net_drv_recv_frame(void *data, uint16_t cap, uint16_t *len_out);
int net_drv_mac(uint8_t out[6]);
int net_drv_ready(void);

#endif
