#include "sock.h"
#include "net.h"
#include "../drivers/net/net_drv.h"
#include "../memory/heap.h"
#include "../proc/sched.h"
#include "../proc/process.h"
#include "../syscall/uaccess.h"

/* A small TCP/IP stack for many concurrent client connections: an ARP cache,
 * TCP with retransmission, flow control and slow start, and UDP datagram
 * sockets.  Frames are pumped from the timer tick and from socket calls. */

#define SOCK_MAX        64
#define TCP_MSS         1460
#define TCP_SBUF        (64 * 1024)
#define TCP_RBUF        65535
#define TCP_RTO_MIN     30      /* ticks (10 ms each) */
#define TCP_RTO_MAX     600
#define TCP_RETRIES     12
#define TCP_TIMEWAIT    100
#define UDP_QUEUE       16
#define ARP_CACHE       16
#define LEGACY_QUEUE    32

#define EAGAIN_      11
#define EBADF_       9
#define EINVAL_      22
#define EMFILE_      24
#define ENOMEM_      12
#define EFAULT_      14
#define EPIPE_       32
#define ECONNREFUSED_ 111
#define ECONNRESET_  104
#define ETIMEDOUT_   110
#define ENOTCONN_    107
#define EHOSTUNREACH_ 113

enum {
    T_FREE = 0, T_ARP, T_SYN_SENT, T_ESTABLISHED, T_FIN_WAIT_1, T_FIN_WAIT_2,
    T_CLOSE_WAIT, T_LAST_ACK, T_TIME_WAIT, T_CLOSED
};

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint16_t len;
    uint8_t *data;
} udp_dgram_t;

typedef struct {
    int       type;
    int       state;
    int       err;
    int       user_closed;
    uint64_t  owner;
    uint32_t  rip;
    uint16_t  rport, lport;
    uint8_t   mac[6];
    uint64_t  arp_deadline;
    /* TCP */
    uint32_t  snd_una, snd_nxt, snd_wnd, iss;
    uint32_t  rcv_nxt;
    uint32_t  cwnd, ssthresh;
    uint8_t  *sbuf;
    uint32_t  slen;           /* bytes in sbuf, starting at snd_una */
    int       fin_queued, fin_sent;
    uint8_t  *rbuf;
    uint32_t  rhead, rlen;
    int       peer_fin;
    uint64_t  rto_deadline;
    uint32_t  rto;
    int       retries;
    uint32_t  last_wnd;
    /* UDP */
    udp_dgram_t q[UDP_QUEUE];
    int       qhead, qlen;
} sock_t;

static sock_t socks[SOCK_MAX];
static uint16_t next_port = 49152;

static struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_cache[ARP_CACHE];
static int arp_next;

static struct {
    uint8_t  frame[NET_FRAME_CAP];
    uint16_t len;
} legacy[LEGACY_QUEUE];
static int legacy_head, legacy_len;
static int pumping;

#include "../drivers/serial/serial.h"

static void trace(const char *what, const sock_t *s, uint32_t a, uint32_t b) {
    static const char hex[] = "0123456789abcdef";
    char buf[96];
    int n = 0;
    const char *w = "[tcp] ";
    while (*w) buf[n++] = *w++;
    while (*what && n < 60) buf[n++] = *what++;
    buf[n++] = ' ';
    for (int i = 12; i >= 0; i -= 4) buf[n++] = hex[(s->lport >> i) & 15];
    buf[n++] = ' ';
    for (int i = 28; i >= 0; i -= 4) buf[n++] = hex[(a >> i) & 15];
    buf[n++] = ' ';
    for (int i = 28; i >= 0; i -= 4) buf[n++] = hex[(b >> i) & 15];
    buf[n++] = 10;
    buf[n] = 0;
    serial_write(buf);
}

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint64_t now(void) {
    return sched_ticks();
}

static uint32_t rand32(void) {
    uint32_t lo, hi;
    static uint32_t s = 0x9E3779B9U;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    s ^= lo ^ (hi << 7);
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

/* ---- ARP ---------------------------------------------------------------- */

static int arp_lookup(uint32_t ip, uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            copy_bytes(mac, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void arp_store(uint32_t ip, const uint8_t mac[6]) {
    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            copy_bytes(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    arp_cache[arp_next].ip = ip;
    copy_bytes(arp_cache[arp_next].mac, mac, 6);
    arp_cache[arp_next].valid = 1;
    arp_next = (arp_next + 1) % ARP_CACHE;
}

static void arp_send(uint16_t op, uint32_t target_ip, const uint8_t target_mac[6]) {
    uint8_t frame[64];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    zero_bytes(frame, sizeof(frame));
    build_eth(eth, op == ARP_OP_REQUEST ? bcast : target_mac, ETH_TYPE_ARP);
    arp->htype_be = htons16(ARP_HTYPE_ETHERNET);
    arp->ptype_be = htons16(ARP_PTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper_be = htons16(op);
    copy_bytes(arp->sha, net_state.mac, 6);
    ip_to_bytes(net_state.ip, arp->spa);
    if (op == ARP_OP_REPLY) copy_bytes(arp->tha, target_mac, 6);
    ip_to_bytes(target_ip, arp->tpa);
    net_drv_send_frame(frame, sizeof(eth_hdr_t) + sizeof(arp_pkt_t));
}

static uint32_t next_hop(uint32_t ip) {
    return ip_same_subnet(ip, net_state.ip, net_state.netmask) ? ip : net_state.gateway;
}

/* Handles ARP; returns 1 if the frame should also be offered to the older
 * fetchers (replies are, so their own ARP waits still work). */
static int arp_input(const uint8_t *frame, uint16_t len) {
    const arp_pkt_t *arp = (const arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    uint8_t ours[4];
    uint32_t sender;
    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) return 0;
    copy_bytes(&sender, arp->spa, 4);
    arp_store(sender, arp->sha);
    ip_to_bytes(net_state.ip, ours);
    if (ntohs16(arp->oper_be) == ARP_OP_REQUEST && arp->tpa[0] == ours[0] && arp->tpa[1] == ours[1] &&
        arp->tpa[2] == ours[2] && arp->tpa[3] == ours[3]) {
        arp_send(ARP_OP_REPLY, sender, arp->sha);
        return 0;
    }
    return 1;
}

/* ---- output ------------------------------------------------------------- */

static int ip_output(sock_t *s, uint8_t proto, const uint8_t *hdr, uint16_t hlen,
                     const uint8_t *payload, uint16_t plen) {
    uint8_t frame[NET_FRAME_CAP];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    uint8_t *l4 = frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t);
    uint16_t ip_len = (uint16_t)(sizeof(ipv4_hdr_t) + hlen + plen);
    uint32_t sum = 0;
    static uint16_t ident;
    if (sizeof(eth_hdr_t) + ip_len > sizeof(frame)) return -1;
    build_eth(eth, s->mac, ETH_TYPE_IPV4);
    zero_bytes(ip, sizeof(*ip));
    ip->ver_ihl = 0x45;
    ip->total_len_be = htons16(ip_len);
    ip->ident_be = htons16(ident++);
    ip->frag_be = htons16(0x4000);
    ip->ttl = 64;
    ip->proto = proto;
    ip->src_be = net_state.ip;
    ip->dst_be = s->rip;
    ip->checksum_be = htons16((uint16_t)ip_checksum(ip, sizeof(ipv4_hdr_t)));
    copy_bytes(l4, hdr, hlen);
    if (plen) copy_bytes(l4 + hlen, payload, plen);
    {
        const uint8_t *a = (const uint8_t *)&ip->src_be;
        uint16_t l4len = (uint16_t)(hlen + plen);
        sum += ((uint32_t)a[0] << 8 | a[1]) + ((uint32_t)a[2] << 8 | a[3]);
        sum += ((uint32_t)a[4] << 8 | a[5]) + ((uint32_t)a[6] << 8 | a[7]);
        sum += proto + l4len;
        for (uint16_t i = 0; i + 1 < l4len; i += 2) sum += (uint32_t)l4[i] << 8 | l4[i + 1];
        if (l4len & 1) sum += (uint32_t)l4[l4len - 1] << 8;
        while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
        sum = ~sum & 0xFFFF;
        if (proto == IP_PROTO_UDP && sum == 0) sum = 0xFFFF;
        if (proto == IP_PROTO_TCP) { l4[16] = (uint8_t)(sum >> 8); l4[17] = (uint8_t)sum; }
        else { l4[6] = (uint8_t)(sum >> 8); l4[7] = (uint8_t)sum; }
    }
    return net_drv_send_frame(frame, (uint16_t)(sizeof(eth_hdr_t) + ip_len));
}

static uint32_t rbuf_free(const sock_t *s) {
    return TCP_RBUF - s->rlen;
}

static void tcp_send(sock_t *s, uint32_t seq, uint8_t flags, const uint8_t *data, uint16_t len) {
    uint8_t h[24];
    uint16_t hlen = (flags & TCP_FLAG_SYN) ? 24 : 20;
    uint32_t wnd = rbuf_free(s);
    zero_bytes(h, sizeof(h));
    h[0] = (uint8_t)(s->lport >> 8);
    h[1] = (uint8_t)s->lport;
    h[2] = (uint8_t)(s->rport >> 8);
    h[3] = (uint8_t)s->rport;
    wr32be(h + 4, seq);
    wr32be(h + 8, (flags & TCP_FLAG_ACK) ? s->rcv_nxt : 0);
    h[12] = (uint8_t)((hlen / 4) << 4);
    h[13] = flags;
    h[14] = (uint8_t)(wnd >> 8);
    h[15] = (uint8_t)wnd;
    if (hlen == 24) {
        h[20] = 2;
        h[21] = 4;
        h[22] = (uint8_t)(TCP_MSS >> 8);
        h[23] = (uint8_t)TCP_MSS;
    }
    s->last_wnd = wnd;
    ip_output(s, IP_PROTO_TCP, h, hlen, data, len);
}

static void tcp_ack(sock_t *s) {
    tcp_send(s, s->snd_nxt, TCP_FLAG_ACK, 0, 0);
}

static void arm_rto(sock_t *s) {
    s->rto_deadline = now() + s->rto;
}

/* Sends whatever the window allows from the send buffer, then the FIN. */
static void tcp_output(sock_t *s) {
    uint32_t wnd, flight;
    if (s->state != T_ESTABLISHED && s->state != T_CLOSE_WAIT && s->state != T_FIN_WAIT_1 &&
        s->state != T_LAST_ACK) {
        return;
    }
    wnd = s->snd_wnd < s->cwnd ? s->snd_wnd : s->cwnd;
    for (;;) {
        uint32_t off = s->snd_nxt - s->snd_una, chunk;
        flight = off;
        if (off >= s->slen || flight >= wnd) break;
        chunk = s->slen - off;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        if (chunk > wnd - flight) chunk = wnd - flight;
        {
            uint8_t seg[TCP_MSS];
            for (uint32_t i = 0; i < chunk; i++) seg[i] = s->sbuf[(off + i) % TCP_SBUF];
            tcp_send(s, s->snd_nxt, TCP_FLAG_ACK | TCP_FLAG_PSH, seg, (uint16_t)chunk);
        }
        if (s->snd_una == s->snd_nxt) arm_rto(s);
        s->snd_nxt += chunk;
    }
    if (s->fin_queued && !s->fin_sent && s->snd_nxt - s->snd_una == s->slen) {
        if (s->snd_una == s->snd_nxt) arm_rto(s);
        tcp_send(s, s->snd_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        s->snd_nxt++;
        s->fin_sent = 1;
        s->state = s->state == T_CLOSE_WAIT ? T_LAST_ACK : T_FIN_WAIT_1;
    }
}

/* ---- input -------------------------------------------------------------- */

static void sock_release(sock_t *s) {
    if (s->sbuf) kfree(s->sbuf);
    if (s->rbuf) kfree(s->rbuf);
    for (int i = 0; i < s->qlen; i++) kfree(s->q[(s->qhead + i) % UDP_QUEUE].data);
    zero_bytes(s, sizeof(*s));
}

static void tcp_fail(sock_t *s, int err) {
    s->state = T_CLOSED;
    s->err = err;
    if (s->user_closed) sock_release(s);
}

static void tcp_input(sock_t *s, const uint8_t *t, uint16_t tlen) {
    uint32_t seq = rd32be(t + 4), ack = rd32be(t + 8);
    uint8_t flags = t[13];
    uint16_t doff = (uint16_t)((t[12] >> 4) * 4);
    const uint8_t *data = t + doff;
    uint16_t dlen;
    if (doff < 20 || doff > tlen) return;
    dlen = (uint16_t)(tlen - doff);
    if (flags & TCP_FLAG_RST) {
        trace("rst", s, seq, s->state);
        if (s->state == T_SYN_SENT ? (ack == s->snd_nxt) : (seq == s->rcv_nxt || dlen == 0)) {
            tcp_fail(s, s->state == T_SYN_SENT ? ECONNREFUSED_ : ECONNRESET_);
        }
        return;
    }
    if (s->state == T_SYN_SENT) {
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) && ack == s->snd_nxt) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_wnd = ((uint32_t)t[14] << 8) | t[15];
            s->state = T_ESTABLISHED;
            s->retries = 0;
            s->rto = TCP_RTO_MIN * 2;
            tcp_ack(s);
            tcp_output(s);
        }
        return;
    }
    if (flags & TCP_FLAG_ACK) {
        uint32_t acked = ack - s->snd_una;
        if (acked && acked <= s->snd_nxt - s->snd_una) {
            uint32_t data_acked = acked;
            if (s->fin_sent && ack == s->snd_nxt) data_acked--;
            if (data_acked > s->slen) data_acked = s->slen;
            s->slen -= data_acked;
            s->snd_una = ack;
            s->retries = 0;
            if (s->cwnd < s->ssthresh) s->cwnd += TCP_MSS;
            else s->cwnd += TCP_MSS * TCP_MSS / s->cwnd + 1;
            if (s->cwnd > TCP_SBUF) s->cwnd = TCP_SBUF;
            if (s->snd_una != s->snd_nxt) arm_rto(s);
            if (s->fin_sent && ack == s->snd_nxt) {
                if (s->state == T_FIN_WAIT_1) s->state = T_FIN_WAIT_2;
                else if (s->state == T_LAST_ACK) {
                    s->state = T_CLOSED;
                    if (s->user_closed) {
                        sock_release(s);
                        return;
                    }
                }
            }
        }
        s->snd_wnd = ((uint32_t)t[14] << 8) | t[15];
    }
    if (dlen || (flags & TCP_FLAG_FIN)) {
        if (seq != s->rcv_nxt) trace("ooo", s, seq, s->rcv_nxt);
        if (seq == s->rcv_nxt && !s->peer_fin) {
            uint32_t take = dlen < rbuf_free(s) ? dlen : rbuf_free(s);
            for (uint32_t i = 0; i < take; i++) s->rbuf[(s->rhead + s->rlen + i) % TCP_RBUF] = data[i];
            s->rlen += take;
            s->rcv_nxt += take;
            if ((flags & TCP_FLAG_FIN) && take == dlen) {
                s->rcv_nxt++;
                s->peer_fin = 1;
                if (s->state == T_ESTABLISHED) s->state = T_CLOSE_WAIT;
                else if (s->state == T_FIN_WAIT_1 || s->state == T_FIN_WAIT_2) {
                    s->state = T_TIME_WAIT;
                    s->rto_deadline = now() + TCP_TIMEWAIT;
                }
            }
        }
        tcp_ack(s);
    }
    tcp_output(s);
}

static void udp_input(sock_t *s, uint32_t src, uint16_t sport, const uint8_t *data, uint16_t len) {
    udp_dgram_t *d;
    if (s->qlen >= UDP_QUEUE) return;
    d = &s->q[(s->qhead + s->qlen) % UDP_QUEUE];
    d->data = (uint8_t *)kmalloc(len ? len : 1);
    if (!d->data) return;
    copy_bytes(d->data, data, len);
    d->len = len;
    d->ip = src;
    d->port = sport;
    s->qlen++;
}

/* Returns 1 if the frame was consumed by a socket. */
static int ip_input(const uint8_t *frame, uint16_t len) {
    const ipv4_hdr_t *ip = (const ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    const uint8_t *l4;
    uint16_t ihl, total, l4len, dport, sport;
    if (len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t)) return 0;
    ihl = (uint16_t)((ip->ver_ihl & 0x0F) * 4);
    total = ntohs16(ip->total_len_be);
    if ((ip->ver_ihl >> 4) != 4 || ihl < 20 || total < ihl || sizeof(eth_hdr_t) + total > len) return 0;
    if (ip->dst_be != net_state.ip) return 0;
    if (ntohs16(ip->frag_be) & 0x3FFF) return 0;
    l4 = (const uint8_t *)ip + ihl;
    l4len = (uint16_t)(total - ihl);
    if (l4len < 8) return 0;
    sport = (uint16_t)((l4[0] << 8) | l4[1]);
    dport = (uint16_t)((l4[2] << 8) | l4[3]);
    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t *s = &socks[i];
        if (!s->state || s->lport != dport) continue;
        if (ip->proto == IP_PROTO_TCP && s->type == SOCK_TCP && s->rip == ip->src_be && s->rport == sport) {
            if (l4len >= 20) tcp_input(s, l4, l4len);
            return 1;
        }
        if (ip->proto == IP_PROTO_UDP && s->type == SOCK_UDP) {
            uint16_t ulen = (uint16_t)((l4[4] << 8) | l4[5]);
            if (ulen >= 8 && ulen <= l4len) udp_input(s, ip->src_be, sport, l4 + 8, (uint16_t)(ulen - 8));
            return 1;
        }
    }
    return 0;
}

static void legacy_push(const uint8_t *frame, uint16_t len) {
    int slot;
    if (legacy_len == LEGACY_QUEUE) {
        legacy_head = (legacy_head + 1) % LEGACY_QUEUE;
        legacy_len--;
    }
    slot = (legacy_head + legacy_len) % LEGACY_QUEUE;
    copy_bytes(legacy[slot].frame, frame, len);
    legacy[slot].len = len;
    legacy_len++;
}

static int any_socket(void) {
    for (int i = 0; i < SOCK_MAX; i++) {
        if (socks[i].state) return 1;
    }
    return 0;
}

/* Drains the NIC: socket traffic goes to the stack, everything else to the
 * queue the older blocking fetchers read. */
static void rx_pump(void) {
    static uint8_t frame[NET_FRAME_CAP];
    uint16_t len;
    if (pumping || !net_ready()) return;
    pumping = 1;
    for (int n = 0; n < 256; n++) {
        int rc = net_drv_recv_frame(frame, sizeof(frame), &len);
        const eth_hdr_t *eth = (const eth_hdr_t *)frame;
        if (rc <= 0) break;
        if (len < sizeof(eth_hdr_t)) continue;
        if (ntohs16(eth->type_be) == ETH_TYPE_ARP) {
            if (arp_input(frame, len)) legacy_push(frame, len);
            continue;
        }
        if (ntohs16(eth->type_be) == ETH_TYPE_IPV4 && ip_input(frame, len)) continue;
        legacy_push(frame, len);
    }
    pumping = 0;
}

int net_rx_frame(void *buf, uint16_t cap, uint16_t *len_out) {
    if (!any_socket() && legacy_len == 0) return net_drv_recv_frame(buf, cap, len_out);
    rx_pump();
    if (legacy_len == 0) return 0;
    {
        uint16_t len = legacy[legacy_head].len;
        if (len > cap) len = cap;
        copy_bytes(buf, legacy[legacy_head].frame, len);
        *len_out = len;
        legacy_head = (legacy_head + 1) % LEGACY_QUEUE;
        legacy_len--;
        return 1;
    }
}

/* ---- timers ------------------------------------------------------------- */

static void tcp_timer(sock_t *s) {
    uint64_t t = now();
    if (s->state == T_ARP) {
        if (arp_lookup(next_hop(s->rip), s->mac)) {
            s->state = T_SYN_SENT;
            tcp_send(s, s->iss, TCP_FLAG_SYN, 0, 0);
            arm_rto(s);
        } else if (t >= s->arp_deadline) {
            tcp_fail(s, EHOSTUNREACH_);
        } else if ((t & 15) == 0) {
            arp_send(ARP_OP_REQUEST, next_hop(s->rip), 0);
        }
        return;
    }
    if (s->state == T_TIME_WAIT) {
        if (t >= s->rto_deadline) {
            s->state = T_CLOSED;
            if (s->user_closed) sock_release(s);
        }
        return;
    }
    if (s->state != T_CLOSED && s->rlen < TCP_RBUF && s->last_wnd < TCP_MSS && rbuf_free(s) >= TCP_MSS) {
        tcp_ack(s);
    }
    if (s->snd_wnd == 0 && s->slen && s->snd_una == s->snd_nxt) {
        /* zero window: probe with one byte until the peer opens it */
        if (!s->rto_deadline) arm_rto(s);
        if (t >= s->rto_deadline) {
            s->snd_wnd = 1;
            tcp_output(s);
            s->snd_wnd = 0;
            arm_rto(s);
        }
        return;
    }
    if (s->snd_una == s->snd_nxt || !s->rto_deadline || t < s->rto_deadline) return;
    if (++s->retries > TCP_RETRIES) {
        tcp_fail(s, ETIMEDOUT_);
        return;
    }
    trace("rexmit", s, s->snd_una, s->state);
    s->rto = s->rto * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : s->rto * 2;
    s->ssthresh = s->cwnd / 2 > 2 * TCP_MSS ? s->cwnd / 2 : 2 * TCP_MSS;
    s->cwnd = TCP_MSS;
    if (s->state == T_SYN_SENT) {
        tcp_send(s, s->iss, TCP_FLAG_SYN, 0, 0);
        arm_rto(s);
        return;
    }
    /* go back to the oldest unacknowledged byte */
    s->snd_nxt = s->snd_una;
    if (s->fin_sent) {
        s->fin_sent = 0;
        s->state = s->state == T_LAST_ACK ? T_CLOSE_WAIT : T_ESTABLISHED;
    }
    tcp_output(s);
    arm_rto(s);
}

void sock_tick(void) {
    if (!any_socket()) return;
    rx_pump();
    for (int i = 0; i < SOCK_MAX; i++) {
        if (socks[i].type == SOCK_TCP && socks[i].state) tcp_timer(&socks[i]);
    }
}

/* ---- socket calls ------------------------------------------------------- */

static sock_t *sock_get(int64_t h) {
    process_t *p = sched_current_process();
    if (h < 1 || h > SOCK_MAX || !p) return 0;
    if (!socks[h - 1].state || socks[h - 1].owner != p->pid || socks[h - 1].user_closed) return 0;
    return &socks[h - 1];
}

static uint16_t alloc_port(void) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t port = next_port++;
        int used = 0;
        if (next_port < 49152) next_port = 49152;
        for (int i = 0; i < SOCK_MAX; i++) {
            if (socks[i].state && socks[i].lport == port) used = 1;
        }
        if (!used) return port;
    }
    return 0;
}

static int64_t op_socket(uint64_t type) {
    process_t *p = sched_current_process();
    if (type != SOCK_TCP && type != SOCK_UDP) return -EINVAL_;
    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t *s = &socks[i];
        if (s->state) continue;
        zero_bytes(s, sizeof(*s));
        s->type = (int)type;
        s->state = T_CLOSED;
        s->owner = p ? p->pid : 0;
        s->lport = alloc_port();
        if (type == SOCK_TCP) {
            s->sbuf = (uint8_t *)kmalloc(TCP_SBUF);
            s->rbuf = (uint8_t *)kmalloc(TCP_RBUF);
            if (!s->sbuf || !s->rbuf) {
                sock_release(s);
                return -ENOMEM_;
            }
        }
        return i + 1;
    }
    return -EMFILE_;
}

static int64_t op_connect(sock_t *s, uint32_t ip, uint16_t port) {
    if (s->type != SOCK_TCP || s->rport) return -EINVAL_;
    if (!net_ready()) return -EHOSTUNREACH_;
    s->rip = ip;
    s->rport = port;
    s->iss = rand32();
    s->snd_una = s->iss;
    s->snd_nxt = s->iss + 1;
    s->cwnd = 10 * TCP_MSS;
    s->ssthresh = TCP_SBUF;
    s->rto = TCP_RTO_MIN * 3;
    s->snd_wnd = TCP_MSS;
    if (arp_lookup(next_hop(ip), s->mac)) {
        s->state = T_SYN_SENT;
        tcp_send(s, s->iss, TCP_FLAG_SYN, 0, 0);
        arm_rto(s);
    } else {
        s->state = T_ARP;
        s->arp_deadline = now() + 300;
        arp_send(ARP_OP_REQUEST, next_hop(ip), 0);
    }
    return 0;
}

static int64_t op_send(sock_t *s, const uint8_t *ubuf, uint64_t len) {
    uint8_t chunk[512];
    uint64_t done = 0;
    if (s->type != SOCK_TCP) return -EINVAL_;
    if (s->state == T_CLOSED) return s->err ? -s->err : -ENOTCONN_;
    if (s->fin_queued) return -EPIPE_;
    if (s->state == T_ARP || s->state == T_SYN_SENT) return -EAGAIN_;
    while (done < len && s->slen < TCP_SBUF) {
        uint64_t n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (n > TCP_SBUF - s->slen) n = TCP_SBUF - s->slen;
        if (copy_from_user(chunk, ubuf + done, n) != 0) return done ? (int64_t)done : -EFAULT_;
        for (uint64_t i = 0; i < n; i++) s->sbuf[(s->slen + i) % TCP_SBUF] = chunk[i];
        s->slen += (uint32_t)n;
        done += n;
    }
    tcp_output(s);
    return done ? (int64_t)done : -EAGAIN_;
}

static int64_t op_recv(sock_t *s, uint8_t *ubuf, uint64_t cap) {
    uint8_t chunk[512];
    uint64_t done = 0;
    if (s->type != SOCK_TCP) return -EINVAL_;
    while (done < cap && s->rlen) {
        uint64_t n = cap - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (n > s->rlen) n = s->rlen;
        for (uint64_t i = 0; i < n; i++) chunk[i] = s->rbuf[(s->rhead + i) % TCP_RBUF];
        if (copy_to_user(ubuf + done, chunk, n) != 0) return done ? (int64_t)done : -EFAULT_;
        s->rhead = (uint32_t)((s->rhead + n) % TCP_RBUF);
        s->rlen -= (uint32_t)n;
        done += n;
    }
    if (done) {
        if (s->last_wnd < TCP_MSS * 2 && rbuf_free(s) >= TCP_RBUF / 2) tcp_ack(s);
        return (int64_t)done;
    }
    if (s->peer_fin) return 0;
    if (s->state == T_CLOSED) return s->err ? -s->err : 0;
    return -EAGAIN_;
}

static int64_t op_close(int64_t h) {
    sock_t *s = sock_get(h);
    if (!s) return -EBADF_;
    s->user_closed = 1;
    if (s->type == SOCK_TCP && (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT)) {
        s->fin_queued = 1;
        tcp_output(s);
        return 0;
    }
    if (s->type == SOCK_TCP && (s->state == T_FIN_WAIT_1 || s->state == T_FIN_WAIT_2 ||
                                s->state == T_LAST_ACK || s->state == T_TIME_WAIT)) {
        return 0;
    }
    if (s->type == SOCK_TCP && s->state == T_SYN_SENT) {
        tcp_send(s, s->snd_nxt, TCP_FLAG_RST, 0, 0);
    }
    sock_release(s);
    return 0;
}

static int64_t op_status(sock_t *s) {
    if (s->type == SOCK_UDP) return NET_ST_OPEN;
    switch (s->state) {
    case T_ARP: case T_SYN_SENT: return NET_ST_CONNECTING;
    case T_CLOSED: return s->err ? -s->err : NET_ST_CLOSED;
    default: return NET_ST_OPEN;
    }
}

static uint16_t ready_mask(sock_t *s) {
    uint16_t m = 0;
    if (!s) return NET_POLL_ERR;
    if (s->type == SOCK_UDP) return (uint16_t)((s->qlen ? NET_POLL_IN : 0) | NET_POLL_OUT);
    if (s->rlen || s->peer_fin) m |= NET_POLL_IN;
    if (s->peer_fin) m |= NET_POLL_HUP;
    if ((s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT) && s->slen < TCP_SBUF) m |= NET_POLL_OUT;
    if (s->state == T_CLOSED) m |= s->err ? (NET_POLL_ERR | NET_POLL_IN) : (NET_POLL_HUP | NET_POLL_IN);
    return m;
}

static int64_t op_poll(net_pollfd_t *ufds, uint64_t n, uint64_t timeout_ms) {
    net_pollfd_t k[SOCK_MAX];
    uint64_t start = now();
    uint64_t limit = timeout_ms == (uint64_t)-1 ? (uint64_t)-1 : (timeout_ms + 9) / 10;
    if (n > SOCK_MAX) return -EINVAL_;
    if (n && copy_from_user(k, ufds, n * sizeof(net_pollfd_t)) != 0) return -EFAULT_;
    for (;;) {
        int64_t ready = 0;
        rx_pump();
        for (uint64_t i = 0; i < n; i++) {
            sock_t *s = sock_get(k[i].handle);
            k[i].revents = (uint16_t)(ready_mask(s) & (k[i].events | NET_POLL_ERR | NET_POLL_HUP));
            if (k[i].revents) ready++;
        }
        if (ready || (limit != (uint64_t)-1 && now() - start >= limit)) {
            if (n && copy_to_user(ufds, k, n * sizeof(net_pollfd_t)) != 0) return -EFAULT_;
            return ready;
        }
        sched_sleep(1);
    }
}

static int64_t op_sendto(sock_t *s, const uint8_t *ubuf, uint64_t len, uint32_t ip, uint16_t port) {
    uint8_t data[1472], h[8];
    if (s->type != SOCK_UDP || len > sizeof(data)) return -EINVAL_;
    if (copy_from_user(data, ubuf, len) != 0) return -EFAULT_;
    s->rip = ip;
    if (!arp_lookup(next_hop(ip), s->mac)) {
        uint64_t deadline = now() + 100;
        arp_send(ARP_OP_REQUEST, next_hop(ip), 0);
        while (!arp_lookup(next_hop(ip), s->mac)) {
            if (now() >= deadline) return -EHOSTUNREACH_;
            sched_sleep(1);
            rx_pump();
        }
    }
    h[0] = (uint8_t)(s->lport >> 8);
    h[1] = (uint8_t)s->lport;
    h[2] = (uint8_t)(port >> 8);
    h[3] = (uint8_t)port;
    h[4] = (uint8_t)((len + 8) >> 8);
    h[5] = (uint8_t)(len + 8);
    h[6] = h[7] = 0;
    return ip_output(s, IP_PROTO_UDP, h, 8, data, (uint16_t)len) == 0 ? (int64_t)len : -EHOSTUNREACH_;
}

static int64_t op_recvfrom(sock_t *s, uint8_t *ubuf, uint64_t cap, net_addr_t *uaddr) {
    udp_dgram_t *d;
    uint64_t n;
    if (s->type != SOCK_UDP) return -EINVAL_;
    rx_pump();
    if (!s->qlen) return -EAGAIN_;
    d = &s->q[s->qhead];
    n = d->len < cap ? d->len : cap;
    if (copy_to_user(ubuf, d->data, n) != 0) return -EFAULT_;
    if (uaddr) {
        net_addr_t a;
        a.ip = d->ip;
        a.port = d->port;
        a.pad = 0;
        if (copy_to_user(uaddr, &a, sizeof(a)) != 0) return -EFAULT_;
    }
    kfree(d->data);
    s->qhead = (s->qhead + 1) % UDP_QUEUE;
    s->qlen--;
    return (int64_t)n;
}

int64_t sock_syscall(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    sock_t *s;
    if (op == NET_OP_SOCKET) return op_socket(a);
    if (op == NET_OP_CLOSE) return op_close((int64_t)a);
    if (op == NET_OP_POLL) return op_poll((net_pollfd_t *)(uintptr_t)a, b, c);
    if (op == NET_OP_INFO) {
        net_info_t info;
        zero_bytes(&info, sizeof(info));
        info.ip = net_state.ip;
        info.gateway = net_state.gateway;
        info.netmask = net_state.netmask;
        info.dns = net_state.dns;
        copy_bytes(info.mac, net_state.mac, 6);
        info.ready = (uint8_t)net_ready();
        return copy_to_user((void *)(uintptr_t)a, &info, sizeof(info)) ? -EFAULT_ : 0;
    }
    s = sock_get((int64_t)a);
    if (!s) return -EBADF_;
    rx_pump();
    switch (op) {
    case NET_OP_CONNECT: return op_connect(s, (uint32_t)b, (uint16_t)c);
    case NET_OP_SEND: return op_send(s, (const uint8_t *)(uintptr_t)b, c);
    case NET_OP_RECV: return op_recv(s, (uint8_t *)(uintptr_t)b, c);
    case NET_OP_STATUS: return op_status(s);
    case NET_OP_SENDTO: return op_sendto(s, (const uint8_t *)(uintptr_t)b, c, (uint32_t)d, (uint16_t)e);
    case NET_OP_RECVFROM: return op_recvfrom(s, (uint8_t *)(uintptr_t)b, c, (net_addr_t *)(uintptr_t)d);
    default: return -EINVAL_;
    }
}

void sock_proc_exit(struct process *proc) {
    for (int i = 0; proc && i < SOCK_MAX; i++) {
        sock_t *s = &socks[i];
        if (!s->state || s->owner != proc->pid || s->user_closed) continue;
        s->user_closed = 1;
        if (s->type == SOCK_TCP && (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT)) {
            s->fin_queued = 1;
            tcp_output(s);
        } else if (!(s->type == SOCK_TCP && (s->state == T_FIN_WAIT_1 || s->state == T_FIN_WAIT_2 ||
                                             s->state == T_LAST_ACK || s->state == T_TIME_WAIT))) {
            sock_release(s);
        }
    }
}
