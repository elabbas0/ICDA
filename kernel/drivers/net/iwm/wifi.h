#ifndef ICDA_WIFI_H
#define ICDA_WIFI_H

#include <stdint.h>

/*
 * Wi-Fi (Intel Wireless 8260 via the iwm(4) port) as an ICDA network
 * driver.  wifi_init() finds the card and starts the driver thread; the
 * net_drv_* entry points below may be called from any context, including
 * the timer interrupt (they only touch frame rings).
 */
int	wifi_init(void);
int	wifi_present(void);
int	wifi_send_frame(const void *data, uint16_t len);
int	wifi_recv_frame(void *data, uint16_t cap, uint16_t *len_out);
int	wifi_mac(uint8_t out[6]);
int	wifi_ready(void);	/* associated and the data port is open */

/* /dev/wifi: writes are commands, reads return the selected view. */
uint64_t wifi_node_read(char *buf, uint64_t cap);
uint64_t wifi_node_write(const char *buf, uint64_t len);

#endif
