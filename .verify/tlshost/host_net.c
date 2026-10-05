/* Linux shim for Surfer's net.h so tls.c/http.c can be debugged natively. */
#define _GNU_SOURCE
#include "net.h"
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

uint64_t icda_ticks(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 100 + t.tv_nsec / 10000000; }
void icda_sleep(uint64_t t) { usleep(t * 10000); }
uint64_t sched_ticks(void) { return icda_ticks(); }

int net_resolve(const char *host, uint32_t *ip) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, 0, &hints, &res) != 0) return -1;
    *ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 0;
}
int net_connect(uint32_t ip, uint16_t port, int timeout_ticks) {
    struct sockaddr_in a = {0};
    int s = socket(AF_INET, SOCK_STREAM, 0);
    (void)timeout_ticks;
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = ip;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) { close(s); return -111; }
    fcntl(s, F_SETFL, O_NONBLOCK);
    return s;
}
int net_connect_start(uint32_t ip, uint16_t port) { return net_connect(ip, port, 0); }
int net_status(int h) { (void)h; return 2; }
long net_send(int h, const void *b, uint32_t n) { long r = send(h, b, n, 0); return r < 0 ? (errno == EAGAIN ? NET_EAGAIN : -1) : r; }
int net_send_all(int h, const uint8_t *b, uint32_t n) { uint32_t d = 0; while (d < n) { long r = net_send(h, b + d, n - d); if (r > 0) d += r; else if (r == NET_EAGAIN) net_wait(h, NET_POLL_OUT, 100); else return -1; } return 0; }
long net_recv(int h, void *b, uint32_t n) { long r = recv(h, b, n, 0); return r < 0 ? (errno == EAGAIN ? NET_EAGAIN : -1) : r; }
int net_wait(int h, int ev, int ms) { struct pollfd p = { h, (short)((ev & 1 ? POLLIN : 0) | (ev & 4 ? POLLOUT : 0)), 0 }; return poll(&p, 1, ms); }
int net_poll(net_pollfd_t *f, int n, int ms) { (void)f; (void)n; (void)ms; return 0; }
void net_close(int h) { close(h); }
long icda_write_file(const char *p, const void *b, unsigned long n) { (void)p; (void)b; return (long)n; }
