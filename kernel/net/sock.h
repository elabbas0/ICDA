#ifndef SOCK_H
#define SOCK_H

#include <stdint.h>

/* Kernel TCP/UDP sockets for native programs (SYS_NET).  IPv4 addresses are
 * kept in wire order, as everywhere in kernel/net. */

#define SOCK_TCP 1
#define SOCK_UDP 2

/* SYS_NET operations */
#define NET_OP_SOCKET   1   /* (type) -> handle */
#define NET_OP_CONNECT  2   /* (h, ip, port) -> 0, completes asynchronously */
#define NET_OP_SEND     3   /* (h, buf, len) -> bytes queued */
#define NET_OP_RECV     4   /* (h, buf, cap) -> bytes, 0 = end of stream, -EAGAIN */
#define NET_OP_CLOSE    5   /* (h) */
#define NET_OP_POLL     6   /* (net_pollfd_t *, n, timeout_ms) -> ready count */
#define NET_OP_STATUS   7   /* (h) -> NET_ST_* or -errno */
#define NET_OP_SENDTO   8   /* (h, buf, len, ip, port) */
#define NET_OP_RECVFROM 9   /* (h, buf, cap, net_addr_t *) */
#define NET_OP_INFO     10  /* (net_info_t *) */

#define NET_ST_CONNECTING 1
#define NET_ST_OPEN       2
#define NET_ST_CLOSED     3

#define NET_POLL_IN   1
#define NET_POLL_OUT  4
#define NET_POLL_ERR  8
#define NET_POLL_HUP  16

typedef struct {
    int32_t  handle;
    uint16_t events;
    uint16_t revents;
} net_pollfd_t;

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint16_t pad;
} net_addr_t;

typedef struct {
    uint32_t ip, gateway, netmask, dns;
    uint8_t  mac[6];
    uint8_t  ready;
    uint8_t  pad;
} net_info_t;

struct process;

int64_t sock_syscall(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
void    sock_tick(void);
void    sock_proc_exit(struct process *proc);

/* Shared receive path: frames the socket layer does not consume are kept
 * for the older blocking fetchers in net.c. */
int     net_rx_frame(void *buf, uint16_t cap, uint16_t *len_out);

#endif
