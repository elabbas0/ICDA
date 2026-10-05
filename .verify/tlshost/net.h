#ifndef SURFER_NET_H
#define SURFER_NET_H

#include <stdint.h>
#include "icda_sys.h"

/* Thin wrappers over SYS_NET (kernel TCP/UDP sockets). */

#define NET_EAGAIN   (-11)
#define NET_POLL_IN  1
#define NET_POLL_OUT 4
#define NET_POLL_ERR 8
#define NET_POLL_HUP 16

typedef struct {
    int32_t  handle;
    uint16_t events;
    uint16_t revents;
} net_pollfd_t;

int  net_resolve(const char *host, uint32_t *ip_out);
int  net_connect(uint32_t ip, uint16_t port, int timeout_ticks);
int  net_connect_start(uint32_t ip, uint16_t port);
int  net_status(int h);
int  net_send_all(int h, const uint8_t *buf, uint32_t len);
long net_send(int h, const void *buf, uint32_t len);
long net_recv(int h, void *buf, uint32_t cap);
int  net_wait(int h, int events, int timeout_ms);
int  net_poll(net_pollfd_t *fds, int n, int timeout_ms);
void net_close(int h);

#endif
