#ifndef E1000_H
#define E1000_H

#include <stdint.h>

int e1000_init(void);
int e1000_ready(void);
uint32_t e1000_last_error(void);
int e1000_mac(uint8_t out[6]);
int e1000_send_frame(const void *data, uint16_t len);
int e1000_recv_frame(void *data, uint16_t cap, uint16_t *len_out);

#endif
