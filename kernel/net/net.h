#ifndef ICDA_NET_H
#define ICDA_NET_H

#include <stdint.h>

int net_init(void);
int net_ready(void);
uint32_t net_last_error(void);
int net_http_get_ipv4(uint32_t ipv4_addr, uint16_t port, const char *path, const char *out_path, uint64_t *bytes_out);

#endif
