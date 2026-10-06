#ifndef NET_DRV_H
#define NET_DRV_H

#include <stdint.h>







int net_drv_init(void);
/* set from the kernel command line (icda.nowifi=1) */
extern int net_drv_skip_wifi;
int net_drv_send_frame(const void *data, uint16_t len);
int net_drv_recv_frame(void *data, uint16_t cap, uint16_t *len_out);
int net_drv_mac(uint8_t out[6]);
int net_drv_ready(void);
int net_drv_is_wireless(void);

#endif
