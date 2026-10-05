#include "net.h"
#include <string.h>

#define OP_SOCKET  1
#define OP_CONNECT 2
#define OP_SEND    3
#define OP_RECV    4
#define OP_CLOSE   5
#define OP_POLL    6
#define OP_STATUS  7
#define OP_SENDTO  8
#define OP_RECVFROM 9
#define OP_INFO    10

/* rsa.c (shared with the kernel) asks for the tick counter by this name */
uint64_t sched_ticks(void) {
    return icda_ticks();
}

#define DNS_CACHE 32

static struct {
    char     host[128];
    uint32_t ip;
} dns_cache[DNS_CACHE];
static int dns_next;

static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t parts[4];
    int n = 0;
    for (;;) {
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s++ - '0');
            digits++;
        }
        if (!digits || v > 255 || n == 4) return -1;
        parts[n++] = v;
        if (*s == '.') {
            s++;
            continue;
        }
        break;
    }
    if (*s || n != 4) return -1;
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

/* DNS A lookup over a UDP socket: the DHCP-provided server first, then
 * public resolvers.  Follows CNAMEs by taking the first A record. */
static int dns_ask(uint32_t server, const char *host, uint32_t *ip_out) {
    uint8_t q[512], a[1500];
    int n = 0, h;
    uint16_t id = (uint16_t)icda_ticks() ^ 0x5A17;
    q[n++] = (uint8_t)(id >> 8);
    q[n++] = (uint8_t)id;
    q[n++] = 0x01;
    q[n++] = 0x00;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    for (const char *p = host; *p;) {
        const char *dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);
        if (len <= 0 || len > 63 || n + len + 6 > (int)sizeof(q)) return -1;
        q[n++] = (uint8_t)len;
        memcpy(q + n, p, (size_t)len);
        n += len;
        p += len;
        if (*p == '.') p++;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    h = (int)icda_net(OP_SOCKET, 2, 0, 0, 0, 0);
    if (h < 0) return -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t deadline;
        if (icda_net(OP_SENDTO, (uint64_t)h, (uint64_t)(uintptr_t)q, (uint64_t)n, server, 53) < 0) break;
        deadline = icda_ticks() + 100;
        while (icda_ticks() < deadline) {
            long got = icda_net(OP_RECVFROM, (uint64_t)h, (uint64_t)(uintptr_t)a, sizeof(a), 0, 0);
            int pos, an;
            if (got == NET_EAGAIN) {
                net_wait(h, NET_POLL_IN, 50);
                continue;
            }
            if (got < 12 || a[0] != q[0] || a[1] != q[1]) continue;
            if ((a[3] & 0x0F) != 0) {
                net_close(h);
                return -1;
            }
            an = (a[6] << 8) | a[7];
            pos = n;
            for (int i = 0; i < an && pos < got; i++) {
                int type, rdlen;
                while (pos < got && a[pos] && !(a[pos] & 0xC0)) pos += a[pos] + 1;
                pos += (pos < got && (a[pos] & 0xC0)) ? 2 : 1;
                if (pos + 10 > got) break;
                type = (a[pos] << 8) | a[pos + 1];
                rdlen = (a[pos + 8] << 8) | a[pos + 9];
                pos += 10;
                if (type == 1 && rdlen == 4 && pos + 4 <= got) {
                    memcpy(ip_out, a + pos, 4);
                    net_close(h);
                    return 0;
                }
                pos += rdlen;
            }
            net_close(h);
            return -1;
        }
    }
    net_close(h);
    return -1;
}

static int dns_query(const char *host, uint32_t *ip_out) {
    struct { uint32_t ip, gw, mask, dns; uint8_t mac[6], ready, pad; } info;
    static const uint32_t fallback[2] = { 0x08080808U, 0x01010101U };
    if (icda_net(OP_INFO, (uint64_t)(uintptr_t)&info, 0, 0, 0, 0) == 0 && info.dns &&
        dns_ask(info.dns, host, ip_out) == 0) {
        return 0;
    }
    for (int i = 0; i < 2; i++) {
        if (dns_ask(fallback[i], host, ip_out) == 0) return 0;
    }
    return (long)icda_dns_resolve(host, ip_out) == 0 && *ip_out ? 0 : -1;
}

int net_resolve(const char *host, uint32_t *ip_out) {
    if (parse_ipv4(host, ip_out) == 0) return 0;
    for (int i = 0; i < DNS_CACHE; i++) {
        if (dns_cache[i].ip && strcmp(dns_cache[i].host, host) == 0) {
            *ip_out = dns_cache[i].ip;
            return 0;
        }
    }
    if (dns_query(host, ip_out) != 0) return -1;
    if (strlen(host) < sizeof(dns_cache[0].host)) {
        strcpy(dns_cache[dns_next].host, host);
        dns_cache[dns_next].ip = *ip_out;
        dns_next = (dns_next + 1) % DNS_CACHE;
    }
    return 0;
}

int net_connect_start(uint32_t ip, uint16_t port) {
    long h = icda_net(OP_SOCKET, 1, 0, 0, 0, 0);
    if (h < 0) return (int)h;
    if (icda_net(OP_CONNECT, (uint64_t)h, ip, port, 0, 0) < 0) {
        icda_net(OP_CLOSE, (uint64_t)h, 0, 0, 0, 0);
        return -1;
    }
    return (int)h;
}

int net_status(int h) {
    return (int)icda_net(OP_STATUS, (uint64_t)h, 0, 0, 0, 0);
}

/* Connects and waits up to timeout_ticks; returns the handle or -errno. */
int net_connect(uint32_t ip, uint16_t port, int timeout_ticks) {
    int h = net_connect_start(ip, port);
    uint64_t deadline = icda_ticks() + (uint64_t)timeout_ticks;
    if (h < 0) return h;
    for (;;) {
        int st = net_status(h);
        if (st == 2) return h;
        if (st < 0 || st == 3 || icda_ticks() >= deadline) {
            net_close(h);
            return st < 0 ? st : -110;
        }
        net_wait(h, NET_POLL_OUT, 50);
    }
}

long net_send(int h, const void *buf, uint32_t len) {
    return icda_net(OP_SEND, (uint64_t)h, (uint64_t)(uintptr_t)buf, len, 0, 0);
}

int net_send_all(int h, const uint8_t *buf, uint32_t len) {
    uint32_t done = 0;
    uint64_t deadline = icda_ticks() + 3000;
    while (done < len) {
        long n = net_send(h, buf + done, len - done);
        if (n > 0) {
            done += (uint32_t)n;
            continue;
        }
        if (n != NET_EAGAIN || icda_ticks() >= deadline) return -1;
        net_wait(h, NET_POLL_OUT, 100);
    }
    return 0;
}

long net_recv(int h, void *buf, uint32_t cap) {
    return icda_net(OP_RECV, (uint64_t)h, (uint64_t)(uintptr_t)buf, cap, 0, 0);
}

int net_poll(net_pollfd_t *fds, int n, int timeout_ms) {
    return (int)icda_net(OP_POLL, (uint64_t)(uintptr_t)fds, (uint64_t)n, (uint64_t)(int64_t)timeout_ms, 0, 0);
}

int net_wait(int h, int events, int timeout_ms) {
    net_pollfd_t p;
    p.handle = h;
    p.events = (uint16_t)events;
    p.revents = 0;
    return net_poll(&p, 1, timeout_ms);
}

void net_close(int h) {
    if (h > 0) icda_net(OP_CLOSE, (uint64_t)h, 0, 0, 0, 0);
}
