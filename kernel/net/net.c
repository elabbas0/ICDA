#include "net.h"

#include "../drivers/net/e1000.h"
#include "../drivers/console/console.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../proc/sched.h"

#include <stdint.h>

#define NET_ERR_NO_NIC             1U
#define NET_ERR_ARP_TIMEOUT        2U
#define NET_ERR_TCP_TIMEOUT        3U
#define NET_ERR_TCP_REFUSED        4U
#define NET_ERR_HTTP_PARSE         5U
#define NET_ERR_HTTP_TOO_LARGE     6U
#define NET_ERR_WRITE_FAILED       7U
#define NET_ERR_URL_PATH           8U

#define ETH_TYPE_ARP               0x0806U
#define ETH_TYPE_IPV4              0x0800U

#define ARP_HTYPE_ETHERNET         1U
#define ARP_PTYPE_IPV4             0x0800U
#define ARP_OP_REQUEST             1U
#define ARP_OP_REPLY               2U

#define IP_PROTO_TCP               6U

#define TCP_FLAG_FIN               0x01U
#define TCP_FLAG_SYN               0x02U
#define TCP_FLAG_PSH               0x08U
#define TCP_FLAG_ACK               0x10U

#define NET_FRAME_CAP              1600U
#define NET_HTTP_CAP               (8U * 1024U * 1024U)
#define NET_LOCAL_IP               0x0F02000AU /* 10.0.2.15 little-endian host order */
#define NET_GATEWAY_IP             0x0202000AU /* 10.0.2.2 */
#define NET_NETMASK                0x00FFFFFFU /* 255.255.255.0 */

typedef struct {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type_be;
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint16_t htype_be;
    uint16_t ptype_be;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper_be;
    uint8_t sha[6];
    uint8_t spa[4];
    uint8_t tha[6];
    uint8_t tpa[4];
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_len_be;
    uint16_t ident_be;
    uint16_t frag_be;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum_be;
    uint32_t src_be;
    uint32_t dst_be;
} __attribute__((packed)) ipv4_hdr_t;

typedef struct {
    uint16_t src_port_be;
    uint16_t dst_port_be;
    uint32_t seq_be;
    uint32_t ack_be;
    uint8_t data_offset;
    uint8_t flags;
    uint16_t window_be;
    uint16_t checksum_be;
    uint16_t urgent_be;
} __attribute__((packed)) tcp_hdr_t;

typedef struct {
    uint8_t mac[6];
    uint32_t ip;
    uint32_t gateway;
    uint32_t netmask;
    int ready;
} net_state_t;

static net_state_t net_state;
static uint32_t net_error = 0;

static void net_log(const char *text) {
    console_write("[net] ", CONSOLE_STYLE_MUTED);
    console_write(text, CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);
}

static void net_log_u64(const char *label, uint64_t value) {
    console_write("[net] ", CONSOLE_STYLE_MUTED);
    console_write(label, CONSOLE_STYLE_INFO);
    console_write_dec64(value, CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);
}

static uint16_t bswap16(uint16_t value) {
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint32_t bswap32(uint32_t value) {
    return ((value & 0x000000FFU) << 24) |
           ((value & 0x0000FF00U) << 8) |
           ((value & 0x00FF0000U) >> 8) |
           ((value & 0xFF000000U) >> 24);
}

static uint16_t htons16(uint16_t value) { return bswap16(value); }
static uint16_t ntohs16(uint16_t value) { return bswap16(value); }
static uint32_t htonl32(uint32_t value) { return bswap32(value); }
static uint32_t ntohl32(uint32_t value) { return bswap32(value); }

static void copy_bytes(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < size; i++) d[i] = s[i];
}

static void zero_bytes(void *dst, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < size; i++) d[i] = 0;
}

static uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) len++;
    return len;
}

static int str_prefix(const char *text, const char *prefix) {
    uint64_t i = 0;
    while (prefix[i]) {
        if (text[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static uint32_t ip_checksum(const void *data, uint64_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;
    for (uint64_t i = 0; i + 1 < size; i += 2) {
        sum += ((uint32_t)bytes[i] << 8) | bytes[i + 1];
    }
    if (size & 1U) sum += ((uint32_t)bytes[size - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)(~sum);
}

static uint16_t tcp_checksum(const ipv4_hdr_t *ip, const tcp_hdr_t *tcp, const uint8_t *payload, uint16_t payload_len) {
    uint32_t sum = 0;
    const uint8_t *tcp_bytes = (const uint8_t *)tcp;
    const uint8_t *src_ip = (const uint8_t *)&ip->src_be;
    const uint8_t *dst_ip = (const uint8_t *)&ip->dst_be;
    uint16_t tcp_len = (uint16_t)(sizeof(tcp_hdr_t) + payload_len);

    sum += ((uint32_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint32_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint32_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint32_t)dst_ip[2] << 8) | dst_ip[3];
    sum += IP_PROTO_TCP;
    sum += tcp_len;

    for (uint16_t i = 0; i + 1 < sizeof(tcp_hdr_t); i += 2) {
        if (i == 16) continue;
        sum += ((uint32_t)tcp_bytes[i] << 8) | tcp_bytes[i + 1];
    }

    for (uint16_t i = 0; i + 1 < payload_len; i += 2) {
        sum += ((uint32_t)payload[i] << 8) | payload[i + 1];
    }
    if (payload_len & 1U) sum += ((uint32_t)payload[payload_len - 1] << 8);

    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)(~sum);
}

static int ip_same_subnet(uint32_t a, uint32_t b, uint32_t mask) {
    return (a & mask) == (b & mask);
}

static void ip_to_bytes(uint32_t ip, uint8_t out[4]) {
    out[0] = (uint8_t)(ip & 0xFFU);
    out[1] = (uint8_t)((ip >> 8) & 0xFFU);
    out[2] = (uint8_t)((ip >> 16) & 0xFFU);
    out[3] = (uint8_t)((ip >> 24) & 0xFFU);
}

static void build_eth(eth_hdr_t *eth, const uint8_t dst[6], uint16_t type) {
    copy_bytes(eth->dst, dst, 6);
    copy_bytes(eth->src, net_state.mac, 6);
    eth->type_be = htons16(type);
}

static int send_arp_request(uint32_t target_ip) {
    uint8_t frame[64];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    arp_pkt_t *arp = (arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    uint8_t our_ip[4];
    uint8_t target[4];

    zero_bytes(frame, sizeof(frame));
    build_eth(eth, bcast, ETH_TYPE_ARP);
    arp->htype_be = htons16(ARP_HTYPE_ETHERNET);
    arp->ptype_be = htons16(ARP_PTYPE_IPV4);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper_be = htons16(ARP_OP_REQUEST);
    copy_bytes(arp->sha, net_state.mac, 6);
    ip_to_bytes(net_state.ip, our_ip);
    ip_to_bytes(target_ip, target);
    copy_bytes(arp->spa, our_ip, 4);
    zero_bytes(arp->tha, 6);
    copy_bytes(arp->tpa, target, 4);
    return e1000_send_frame(frame, sizeof(eth_hdr_t) + sizeof(arp_pkt_t));
}

static int try_parse_arp_reply(const uint8_t *frame, uint16_t len, uint32_t target_ip, uint8_t mac_out[6]) {
    const eth_hdr_t *eth;
    const arp_pkt_t *arp;
    uint8_t target[4];

    if (len < sizeof(eth_hdr_t) + sizeof(arp_pkt_t)) return 0;
    eth = (const eth_hdr_t *)frame;
    if (ntohs16(eth->type_be) != ETH_TYPE_ARP) return 0;
    arp = (const arp_pkt_t *)(frame + sizeof(eth_hdr_t));
    if (ntohs16(arp->oper_be) != ARP_OP_REPLY) return 0;
    ip_to_bytes(target_ip, target);
    if (arp->spa[0] != target[0] || arp->spa[1] != target[1] || arp->spa[2] != target[2] || arp->spa[3] != target[3]) {
        return 0;
    }
    copy_bytes(mac_out, arp->sha, 6);
    return 1;
}

static int net_arp_resolve(uint32_t target_ip, uint8_t mac_out[6]) {
    uint8_t frame[NET_FRAME_CAP];
    uint16_t len = 0;
    uint64_t deadline;

    if (send_arp_request(target_ip) != 0) return -1;
    net_log("arp request sent");
    deadline = sched_ticks() + 50;
    while (sched_ticks() < deadline) {
        int rc = e1000_recv_frame(frame, sizeof(frame), &len);
        if (rc < 0) return -1;
        if (rc > 0 && try_parse_arp_reply(frame, len, target_ip, mac_out)) {
            net_log("arp reply received");
            return 0;
        }
        sched_sleep(1);
    }
    return -1;
}

static int send_tcp_packet(const uint8_t dst_mac[6], uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                           uint32_t seq, uint32_t ack, uint8_t flags, const uint8_t *payload, uint16_t payload_len) {
    uint8_t frame[NET_FRAME_CAP];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    tcp_hdr_t *tcp = (tcp_hdr_t *)(frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
    uint8_t *data = frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t);
    uint16_t ip_len = (uint16_t)(sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t) + payload_len);
    uint16_t frame_len = (uint16_t)(sizeof(eth_hdr_t) + ip_len);

    if (frame_len > sizeof(frame)) return -1;
    zero_bytes(frame, frame_len);
    build_eth(eth, dst_mac, ETH_TYPE_IPV4);

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len_be = htons16(ip_len);
    ip->ident_be = htons16((uint16_t)seq);
    ip->frag_be = 0;
    ip->ttl = 64;
    ip->proto = IP_PROTO_TCP;
    ip->src_be = net_state.ip;
    ip->dst_be = dst_ip;
    ip->checksum_be = 0;
    ip->checksum_be = htons16((uint16_t)ip_checksum(ip, sizeof(ipv4_hdr_t)));

    tcp->src_port_be = htons16(src_port);
    tcp->dst_port_be = htons16(dst_port);
    tcp->seq_be = ((seq & 0x000000FFU) << 24) |
                  ((seq & 0x0000FF00U) << 8) |
                  ((seq & 0x00FF0000U) >> 8) |
                  ((seq & 0xFF000000U) >> 24);
    tcp->ack_be = ((ack & 0x000000FFU) << 24) |
                  ((ack & 0x0000FF00U) << 8) |
                  ((ack & 0x00FF0000U) >> 8) |
                  ((ack & 0xFF000000U) >> 24);
    tcp->data_offset = (uint8_t)(sizeof(tcp_hdr_t) / 4U) << 4;
    tcp->flags = flags;
    tcp->window_be = htons16(4096);
    tcp->urgent_be = 0;
    if (payload_len) copy_bytes(data, payload, payload_len);
    tcp->checksum_be = htons16(tcp_checksum(ip, tcp, payload, payload_len));

    return e1000_send_frame(frame, frame_len);
}

typedef struct {
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;
    const uint8_t *payload;
    uint16_t payload_len;
} tcp_packet_info_t;

static int parse_tcp_packet(const uint8_t *frame, uint16_t len, uint32_t expect_src_ip, uint16_t expect_src_port,
                            uint16_t expect_dst_port, tcp_packet_info_t *out) {
    const eth_hdr_t *eth;
    const ipv4_hdr_t *ip;
    const tcp_hdr_t *tcp;
    uint16_t ip_len;
    uint16_t ip_hdr_len;
    uint16_t tcp_hdr_len;
    uint16_t total_hdr;

    if (!frame || !out || len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t)) return 0;
    eth = (const eth_hdr_t *)frame;
    if (ntohs16(eth->type_be) != ETH_TYPE_IPV4) return 0;
    ip = (const ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    if ((ip->ver_ihl >> 4) != 4 || ip->proto != IP_PROTO_TCP) return 0;
    if (ip->src_be != expect_src_ip || ip->dst_be != net_state.ip) return 0;
    ip_hdr_len = (uint16_t)((ip->ver_ihl & 0x0FU) * 4U);
    if (ip_hdr_len < sizeof(ipv4_hdr_t)) return 0;
    ip_len = ntohs16(ip->total_len_be);
    if (ip_len + sizeof(eth_hdr_t) > len) return 0;
    tcp = (const tcp_hdr_t *)((const uint8_t *)ip + ip_hdr_len);
    if (ntohs16(tcp->src_port_be) != expect_src_port || ntohs16(tcp->dst_port_be) != expect_dst_port) return 0;
    tcp_hdr_len = (uint16_t)((tcp->data_offset >> 4) * 4U);
    if (tcp_hdr_len < sizeof(tcp_hdr_t)) return 0;
    total_hdr = sizeof(eth_hdr_t) + ip_hdr_len + tcp_hdr_len;
    if (total_hdr > len) return 0;
    if ((uint16_t)(ip_hdr_len + tcp_hdr_len) > ip_len) return 0;

    out->seq = ntohl32(tcp->seq_be);
    out->ack = ntohl32(tcp->ack_be);
    out->flags = tcp->flags;
    out->payload = frame + total_hdr;
    out->payload_len = (uint16_t)(ip_len - ip_hdr_len - tcp_hdr_len);
    return 1;
}

static void debug_dump_tcp_frame(const uint8_t *frame, uint16_t len) {
    const eth_hdr_t *eth;
    const ipv4_hdr_t *ip;
    const tcp_hdr_t *tcp;
    uint16_t ip_hdr_len;
    uint16_t tcp_hdr_len;
    uint32_t src_ip;
    uint32_t dst_ip;

    if (!frame || len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(tcp_hdr_t)) return;
    eth = (const eth_hdr_t *)frame;
    if (ntohs16(eth->type_be) != ETH_TYPE_IPV4) return;
    ip = (const ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    if ((ip->ver_ihl >> 4) != 4 || ip->proto != IP_PROTO_TCP) return;
    ip_hdr_len = (uint16_t)((ip->ver_ihl & 0x0FU) * 4U);
    if (ip_hdr_len < sizeof(ipv4_hdr_t)) return;
    tcp = (const tcp_hdr_t *)((const uint8_t *)ip + ip_hdr_len);
    tcp_hdr_len = (uint16_t)((tcp->data_offset >> 4) * 4U);
    if (tcp_hdr_len < sizeof(tcp_hdr_t)) return;
    src_ip = ip->src_be;
    dst_ip = ip->dst_be;
    net_log_u64("rx any tcp srcip=", src_ip);
    net_log_u64("rx any tcp dstip=", dst_ip);
    net_log_u64("rx any tcp sport=", ntohs16(tcp->src_port_be));
    net_log_u64("rx any tcp dport=", ntohs16(tcp->dst_port_be));
    net_log_u64("rx any tcp flags=", tcp->flags);
}

static int tcp_connect(const uint8_t dst_mac[6], uint32_t dst_ip, uint16_t dst_port,
                       uint16_t src_port, uint32_t *seq_io, uint32_t *ack_io) {
    uint8_t frame[NET_FRAME_CAP];
    uint16_t len = 0;
    uint64_t deadline;
    tcp_packet_info_t pkt;

    if (send_tcp_packet(dst_mac, dst_ip, src_port, dst_port, *seq_io, 0, TCP_FLAG_SYN, 0, 0) != 0) {
        return -1;
    }
    net_log("tcp syn sent");
    deadline = sched_ticks() + 200;
    while (sched_ticks() < deadline) {
        int rc = e1000_recv_frame(frame, sizeof(frame), &len);
        if (rc < 0) return -1;
        if (rc > 0) debug_dump_tcp_frame(frame, len);
        if (rc > 0 && parse_tcp_packet(frame, len, dst_ip, dst_port, src_port, &pkt)) {
            net_log_u64("tcp flags=", pkt.flags);
            if ((pkt.flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) && pkt.ack == (*seq_io + 1U)) {
                net_log("tcp synack received");
                *ack_io = pkt.seq + 1U;
                *seq_io = *seq_io + 1U;
                if (send_tcp_packet(dst_mac, dst_ip, src_port, dst_port, *seq_io, *ack_io, TCP_FLAG_ACK, 0, 0) != 0) {
                    return -1;
                }
                return 0;
            }
            if (pkt.flags & TCP_FLAG_FIN) return -1;
            if (pkt.flags & 0x04U) return -2;
        }
        sched_sleep(1);
    }
    return -1;
}

static int find_header_end(const uint8_t *buf, uint64_t size) {
    for (uint64_t i = 0; i + 3 < size; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return (int)(i + 4);
    }
    return -1;
}

static int http_status_code(const uint8_t *buf, uint64_t size) {
    if (size < 12) return -1;
    if (!str_prefix((const char *)buf, "HTTP/1.")) return -1;
    if (buf[8] != ' ' || buf[9] < '0' || buf[9] > '9' || buf[10] < '0' || buf[10] > '9' || buf[11] < '0' || buf[11] > '9') {
        return -1;
    }
    return (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
}

int net_init(void) {
    zero_bytes(&net_state, sizeof(net_state));
    if (e1000_init() != 0) {
        net_error = e1000_last_error() ? (100U + e1000_last_error()) : NET_ERR_NO_NIC;
        return -1;
    }
    if (e1000_mac(net_state.mac) != 0) {
        net_error = NET_ERR_NO_NIC;
        return -1;
    }
    net_state.ip = NET_LOCAL_IP;
    net_state.gateway = NET_GATEWAY_IP;
    net_state.netmask = NET_NETMASK;
    net_state.ready = 1;
    net_error = 0;
    return 0;
}

int net_ready(void) {
    return net_state.ready;
}

uint32_t net_last_error(void) {
    return net_error;
}

int net_http_get_ipv4(uint32_t ipv4_addr, uint16_t port, const char *path, const char *out_path, uint64_t *bytes_out) {
    uint8_t *rx_body = 0;
    uint64_t rx_size = 0;
    uint8_t dst_mac[6];
    uint32_t arp_target;
    uint32_t seq = 0x12345678U;
    uint32_t ack = 0;
    uint16_t src_port = 40000;
    char *request = 0;
    uint64_t path_len;
    uint64_t req_len;
    uint8_t frame[NET_FRAME_CAP];
    uint16_t len = 0;
    uint64_t deadline;
    tcp_packet_info_t pkt;
    int fin_seen = 0;
    int header_end;
    int status;

    if (!net_state.ready || !path || !out_path || path[0] != '/') {
        net_error = NET_ERR_URL_PATH;
        return -1;
    }

    arp_target = ip_same_subnet(net_state.ip, ipv4_addr, net_state.netmask) ? ipv4_addr : net_state.gateway;
    if (net_arp_resolve(arp_target, dst_mac) != 0) {
        net_error = NET_ERR_ARP_TIMEOUT;
        return -1;
    }
    net_log("http connect begin");

    {
        int connect_rc = tcp_connect(dst_mac, ipv4_addr, port, src_port, &seq, &ack);
        if (connect_rc == -2) {
            net_error = NET_ERR_TCP_REFUSED;
            return -1;
        }
        if (connect_rc != 0) {
            net_error = NET_ERR_TCP_TIMEOUT;
            return -1;
        }
    }

    path_len = str_len(path);
    req_len = 4 + path_len + str_len(" HTTP/1.0\r\nHost: x\r\nConnection: close\r\n\r\n");
    request = (char *)kmalloc((size_t)(req_len + 1));
    if (!request) return -1;
    zero_bytes(request, req_len + 1);
    copy_bytes(request, "GET ", 4);
    copy_bytes(request + 4, path, path_len);
    copy_bytes(request + 4 + path_len, " HTTP/1.0\r\nHost: x\r\nConnection: close\r\n\r\n", str_len(" HTTP/1.0\r\nHost: x\r\nConnection: close\r\n\r\n"));

    if (send_tcp_packet(dst_mac, ipv4_addr, src_port, port, seq, ack, TCP_FLAG_ACK | TCP_FLAG_PSH, (const uint8_t *)request, (uint16_t)req_len) != 0) {
        kfree(request);
        return -1;
    }
    net_log("http request sent");
    seq += (uint32_t)req_len;
    kfree(request);

    rx_body = (uint8_t *)kmalloc(NET_HTTP_CAP);
    if (!rx_body) return -1;

    deadline = sched_ticks() + 1000;
    while (sched_ticks() < deadline) {
        int rc = e1000_recv_frame(frame, sizeof(frame), &len);
        if (rc < 0) {
            kfree(rx_body);
            return -1;
        }
        if (rc == 0) {
            sched_sleep(1);
            continue;
        }
        if (!parse_tcp_packet(frame, len, ipv4_addr, port, src_port, &pkt)) continue;
        net_log_u64("tcp rx flags=", pkt.flags);
        net_log_u64("tcp rx payload=", pkt.payload_len);
        if (pkt.flags & 0x04U) {
            kfree(rx_body);
            net_error = NET_ERR_TCP_REFUSED;
            return -1;
        }
        if (pkt.payload_len && pkt.seq == ack) {
            if (rx_size + pkt.payload_len > NET_HTTP_CAP) {
                kfree(rx_body);
                net_error = NET_ERR_HTTP_TOO_LARGE;
                return -1;
            }
            copy_bytes(rx_body + rx_size, pkt.payload, pkt.payload_len);
            rx_size += pkt.payload_len;
            ack += pkt.payload_len;
            if (send_tcp_packet(dst_mac, ipv4_addr, src_port, port, seq, ack, TCP_FLAG_ACK, 0, 0) != 0) {
                kfree(rx_body);
                return -1;
            }
            deadline = sched_ticks() + 200;
        }
        if (pkt.flags & TCP_FLAG_FIN) {
            ack += 1U;
            (void)send_tcp_packet(dst_mac, ipv4_addr, src_port, port, seq, ack, TCP_FLAG_ACK, 0, 0);
            (void)send_tcp_packet(dst_mac, ipv4_addr, src_port, port, seq, ack, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            fin_seen = 1;
            break;
        }
    }

    if (!fin_seen && rx_size == 0) {
        kfree(rx_body);
        net_error = NET_ERR_TCP_TIMEOUT;
        return -1;
    }

    header_end = find_header_end(rx_body, rx_size);
    if (header_end < 0) {
        kfree(rx_body);
        net_error = NET_ERR_HTTP_PARSE;
        return -1;
    }
    status = http_status_code(rx_body, rx_size);
    if (status < 200 || status >= 300) {
        kfree(rx_body);
        net_error = (uint32_t)(2000 + (status < 0 ? 0 : status));
        return -1;
    }

    if (vfs_write(vfs_root(), out_path, (const char *)(rx_body + header_end), rx_size - (uint64_t)header_end) != 0) {
        kfree(rx_body);
        net_error = NET_ERR_WRITE_FAILED;
        return -1;
    }

    if (bytes_out) *bytes_out = rx_size - (uint64_t)header_end;
    kfree(rx_body);
    net_error = 0;
    return 0;
}
