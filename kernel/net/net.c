#include "net.h"
#include "tls.h"

#include <stdint.h>

net_state_t net_state;
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

uint16_t bswap16(uint16_t value) {
    return (uint16_t)((value << 8) | (value >> 8));
}

uint32_t bswap32(uint32_t value) {
    return ((value & 0x000000FFU) << 24) |
           ((value & 0x0000FF00U) << 8) |
           ((value & 0x00FF0000U) >> 8) |
           ((value & 0xFF000000U) >> 24);
}

uint16_t htons16(uint16_t value) { return bswap16(value); }
uint16_t ntohs16(uint16_t value) { return bswap16(value); }
uint32_t htonl32(uint32_t value) { return bswap32(value); }
uint32_t ntohl32(uint32_t value) { return bswap32(value); }

void copy_bytes(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < size; i++) d[i] = s[i];
}

void zero_bytes(void *dst, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < size; i++) d[i] = 0;
}

uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) len++;
    return len;
}

int str_prefix(const char *text, const char *prefix) {
    uint64_t i = 0;
    while (prefix[i]) {
        if (text[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

uint32_t ip_checksum(const void *data, uint64_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;
    for (uint64_t i = 0; i + 1 < size; i += 2) {
        sum += ((uint32_t)bytes[i] << 8) | bytes[i + 1];
    }
    if (size & 1U) sum += ((uint32_t)bytes[size - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFFU) + (sum >> 16);
    return (uint16_t)(~sum);
}

uint16_t tcp_checksum(const ipv4_hdr_t *ip, const tcp_hdr_t *tcp, const uint8_t *payload, uint16_t payload_len) {
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

int ip_same_subnet(uint32_t a, uint32_t b, uint32_t mask) {
    return (a & mask) == (b & mask);
}

void ip_to_bytes(uint32_t ip, uint8_t out[4]) {
    out[0] = (uint8_t)(ip & 0xFFU);
    out[1] = (uint8_t)((ip >> 8) & 0xFFU);
    out[2] = (uint8_t)((ip >> 16) & 0xFFU);
    out[3] = (uint8_t)((ip >> 24) & 0xFFU);
}

void build_eth(eth_hdr_t *eth, const uint8_t dst[6], uint16_t type) {
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

int net_arp_resolve(uint32_t target_ip, uint8_t mac_out[6]) {
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

int send_tcp_packet(const uint8_t dst_mac[6], uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
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

int send_udp_packet(const uint8_t dst_mac[6], uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                           const uint8_t *payload, uint16_t payload_len) {
    uint8_t frame[NET_FRAME_CAP];
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    ipv4_hdr_t *ip = (ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    udp_hdr_t *udp = (udp_hdr_t *)(frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t));
    uint8_t *data = frame + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t);
    uint16_t ip_len = (uint16_t)(sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + payload_len);
    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + payload_len);
    uint16_t frame_len = (uint16_t)(sizeof(eth_hdr_t) + ip_len);

    if (frame_len > sizeof(frame)) return -1;
    zero_bytes(frame, frame_len);
    build_eth(eth, dst_mac, ETH_TYPE_IPV4);

    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->total_len_be = htons16(ip_len);
    ip->ident_be = 0;
    ip->frag_be = 0;
    ip->ttl = 64;
    ip->proto = IP_PROTO_UDP;
    ip->src_be = net_state.ip;
    ip->dst_be = dst_ip;
    ip->checksum_be = 0;
    ip->checksum_be = htons16((uint16_t)ip_checksum(ip, sizeof(ipv4_hdr_t)));

    udp->src_port_be = htons16(src_port);
    udp->dst_port_be = htons16(dst_port);
    udp->len_be = htons16(udp_len);
    udp->checksum_be = 0;
    if (payload_len) copy_bytes(data, payload, payload_len);
    return e1000_send_frame(frame, frame_len);
}

int parse_tcp_packet(const uint8_t *frame, uint16_t len, uint32_t expect_src_ip, uint16_t expect_src_port,
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

int parse_udp_packet(const uint8_t *frame, uint16_t len, uint32_t expect_src_ip, uint16_t expect_src_port,
                            uint16_t expect_dst_port, udp_packet_info_t *out) {
    const eth_hdr_t *eth;
    const ipv4_hdr_t *ip;
    const udp_hdr_t *udp;
    uint16_t ip_len;
    uint16_t ip_hdr_len;
    uint16_t udp_len;
    uint16_t total_hdr;

    if (!frame || !out || len < sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t)) return 0;
    eth = (const eth_hdr_t *)frame;
    if (ntohs16(eth->type_be) != ETH_TYPE_IPV4) return 0;
    ip = (const ipv4_hdr_t *)(frame + sizeof(eth_hdr_t));
    if ((ip->ver_ihl >> 4) != 4 || ip->proto != IP_PROTO_UDP) return 0;
    if (ip->src_be != expect_src_ip || ip->dst_be != net_state.ip) return 0;
    ip_hdr_len = (uint16_t)((ip->ver_ihl & 0x0FU) * 4U);
    if (ip_hdr_len < sizeof(ipv4_hdr_t)) return 0;
    ip_len = ntohs16(ip->total_len_be);
    if (ip_len + sizeof(eth_hdr_t) > len) return 0;
    udp = (const udp_hdr_t *)((const uint8_t *)ip + ip_hdr_len);
    if (ntohs16(udp->src_port_be) != expect_src_port || ntohs16(udp->dst_port_be) != expect_dst_port) return 0;
    udp_len = ntohs16(udp->len_be);
    if (udp_len < sizeof(udp_hdr_t)) return 0;
    total_hdr = sizeof(eth_hdr_t) + ip_hdr_len + sizeof(udp_hdr_t);
    if (total_hdr > len) return 0;
    if ((uint16_t)(ip_hdr_len + udp_len) > ip_len) return 0;
    out->payload = frame + total_hdr;
    out->payload_len = (uint16_t)(udp_len - sizeof(udp_hdr_t));
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

int tcp_connect(const uint8_t dst_mac[6], uint32_t dst_ip, uint16_t dst_port,
                       uint16_t src_port, uint32_t *seq_io, uint32_t *ack_io) {
    uint8_t frame[NET_FRAME_CAP];
    uint16_t len = 0;
    uint64_t deadline;
    tcp_packet_info_t pkt;

    if (send_tcp_packet(dst_mac, dst_ip, src_port, dst_port, *seq_io, 0, TCP_FLAG_SYN, 0, 0) != 0) {
        return -1;
    }
    net_log("tcp syn sent");
    deadline = sched_ticks() + 500;
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

int find_header_end(const uint8_t *buf, uint64_t size) {
    for (uint64_t i = 0; i + 3 < size; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return (int)(i + 4);
    }
    return -1;
}

int http_status_code(const uint8_t *buf, uint64_t size) {
    if (size < 12) return -1;
    if (!str_prefix((const char *)buf, "HTTP/1.")) return -1;
    if (buf[8] != ' ' || buf[9] < '0' || buf[9] > '9' || buf[10] < '0' || buf[10] > '9' || buf[11] < '0' || buf[11] > '9') {
        return -1;
    }
    return (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
}

static int dns_encode_name(const char *host, uint8_t *out, uint16_t cap, uint16_t *written_out) {
    uint16_t out_len = 0;
    uint16_t label_len = 0;
    uint16_t label_pos = 0;
    uint16_t i = 0;

    if (!host || !host[0] || !out || !written_out) return -1;
    while (host[i]) {
        if (host[i] == '.') {
            if (label_len == 0 || label_len > 63) return -1;
            out[label_pos] = (uint8_t)label_len;
            label_len = 0;
            i++;
            continue;
        }
        if (label_len == 0) {
            if (out_len >= cap) return -1;
            label_pos = out_len++;
        }
        if (out_len >= cap || label_len >= 63) return -1;
        out[out_len++] = (uint8_t)host[i++];
        label_len++;
    }
    if (label_len == 0) return -1;
    if (out_len >= cap) return -1;
    out[label_pos] = (uint8_t)label_len;
    out[out_len++] = 0;
    *written_out = out_len;
    return 0;
}

static int dns_skip_name(const uint8_t *buf, uint16_t size, uint16_t offset, uint16_t *next_out) {
    uint16_t pos = offset;
    uint16_t next = offset;
    uint16_t guard = 0;

    if (!buf || !next_out || pos >= size) return -1;
    while (pos < size && guard++ < 128) {
        uint8_t len = buf[pos];
        if ((len & 0xC0U) == 0xC0U) {
            if (pos + 1 >= size) return -1;
            *next_out = (uint16_t)(pos + 2);
            return 0;
        }
        if (len == 0) {
            *next_out = (uint16_t)(pos + 1);
            return 0;
        }
        pos++;
        if ((uint16_t)(pos + len) > size) return -1;
        pos = (uint16_t)(pos + len);
        next = pos;
    }
    return -1;
}

int net_dns_resolve_ipv4(const char *host, uint32_t *ipv4_out) {
    uint8_t dst_mac[6];
    uint8_t query[512];
    uint8_t frame[NET_FRAME_CAP];
    uint32_t arp_target;
    uint16_t qname_len = 0;
    uint16_t query_len = 0;
    uint16_t txid;
    uint16_t len = 0;
    uint64_t deadline;
    udp_packet_info_t pkt;
    uint16_t pos;
    uint16_t qdcount;
    uint16_t ancount;

    if (!net_state.ready || !host || !host[0] || !ipv4_out) {
        net_error = NET_ERR_DNS_PARSE;
        return -1;
    }
    zero_bytes(query, sizeof(query));
    if (dns_encode_name(host, query + 12, (uint16_t)(sizeof(query) - 16), &qname_len) != 0) {
        net_error = NET_ERR_DNS_PARSE;
        return -1;
    }
    txid = (uint16_t)(0x4000U + (sched_ticks() & 0x3FFFU));
    query[0] = (uint8_t)(txid >> 8);
    query[1] = (uint8_t)(txid & 0xFFU);
    query[2] = 0x01;
    query[5] = 0x01;
    query_len = (uint16_t)(12 + qname_len);
    query[query_len++] = 0x00;
    query[query_len++] = 0x01;
    query[query_len++] = 0x00;
    query[query_len++] = 0x01;

    arp_target = ip_same_subnet(net_state.ip, NET_DNS_IP, net_state.netmask) ? NET_DNS_IP : net_state.gateway;
    if (net_arp_resolve(arp_target, dst_mac) != 0) {
        net_error = NET_ERR_DNS_TIMEOUT;
        return -1;
    }
    if (send_udp_packet(dst_mac, NET_DNS_IP, 53000, 53, query, query_len) != 0) {
        net_error = NET_ERR_DNS_TIMEOUT;
        return -1;
    }

    deadline = sched_ticks() + 800;
    while (sched_ticks() < deadline) {
        int rc = e1000_recv_frame(frame, sizeof(frame), &len);
        if (rc < 0) {
            net_error = NET_ERR_DNS_TIMEOUT;
            return -1;
        }
        if (rc == 0) {
            sched_sleep(1);
            continue;
        }
        if (!parse_udp_packet(frame, len, NET_DNS_IP, 53, 53000, &pkt)) continue;
        if (pkt.payload_len < 12) continue;
        if (pkt.payload[0] != (uint8_t)(txid >> 8) || pkt.payload[1] != (uint8_t)(txid & 0xFFU)) continue;
        if ((pkt.payload[2] & 0x80U) == 0) continue;
        if ((pkt.payload[3] & 0x0FU) != 0) {
            net_error = NET_ERR_DNS_PARSE;
            return -1;
        }

        qdcount = (uint16_t)(((uint16_t)pkt.payload[4] << 8) | pkt.payload[5]);
        ancount = (uint16_t)(((uint16_t)pkt.payload[6] << 8) | pkt.payload[7]);
        pos = 12;
        for (uint16_t i = 0; i < qdcount; i++) {
            if (dns_skip_name(pkt.payload, pkt.payload_len, pos, &pos) != 0) {
                net_error = NET_ERR_DNS_PARSE;
                return -1;
            }
            if ((uint16_t)(pos + 4) > pkt.payload_len) {
                net_error = NET_ERR_DNS_PARSE;
                return -1;
            }
            pos = (uint16_t)(pos + 4);
        }
        for (uint16_t i = 0; i < ancount; i++) {
            uint16_t type;
            uint16_t class_code;
            uint16_t rdlen;
            if (dns_skip_name(pkt.payload, pkt.payload_len, pos, &pos) != 0) {
                net_error = NET_ERR_DNS_PARSE;
                return -1;
            }
            if ((uint16_t)(pos + 10) > pkt.payload_len) {
                net_error = NET_ERR_DNS_PARSE;
                return -1;
            }
            type = (uint16_t)(((uint16_t)pkt.payload[pos] << 8) | pkt.payload[pos + 1]);
            class_code = (uint16_t)(((uint16_t)pkt.payload[pos + 2] << 8) | pkt.payload[pos + 3]);
            rdlen = (uint16_t)(((uint16_t)pkt.payload[pos + 8] << 8) | pkt.payload[pos + 9]);
            pos = (uint16_t)(pos + 10);
            if ((uint16_t)(pos + rdlen) > pkt.payload_len) {
                net_error = NET_ERR_DNS_PARSE;
                return -1;
            }
            if (type == 1 && class_code == 1 && rdlen == 4) {
                *ipv4_out = (uint32_t)pkt.payload[pos] |
                            ((uint32_t)pkt.payload[pos + 1] << 8) |
                            ((uint32_t)pkt.payload[pos + 2] << 16) |
                            ((uint32_t)pkt.payload[pos + 3] << 24);
                net_error = 0;
                return 0;
            }
            pos = (uint16_t)(pos + rdlen);
        }
        net_error = NET_ERR_DNS_PARSE;
        return -1;
    }

    net_error = NET_ERR_DNS_TIMEOUT;
    return -1;
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

int net_https_get_ipv4(uint32_t ipv4_addr, uint16_t port, const char *path, const char *out_path, uint64_t *bytes_out) {
    tls_conn_t *conn = NULL;
    uint64_t path_len = str_len(path);
    char request[512];
    uint8_t rx_buf[NET_HTTP_CAP];
    uint32_t rx_size = 0;
    int header_end;
    int status;
    uint64_t req_len;

    if (!net_state.ready || !path || !out_path || path[0] != '/') {
        net_error = NET_ERR_URL_PATH;
        return -1;
    }

    net_log("https connect begin");

    {
        int tls_rc = tls_connect(&conn, ipv4_addr, port);
        if (tls_rc == -2) { net_error = NET_ERR_ARP_TIMEOUT; return -1; }
        if (tls_rc == -3) { net_error = NET_ERR_TCP_TIMEOUT; return -1; }
        if (tls_rc == -4) { net_error = NET_ERR_TCP_REFUSED; return -1; }
        if (tls_rc != 0)  { net_error = NET_ERR_TLS_HANDSHAKE; return -1; }
    }

    req_len = 4 + path_len + str_len(" HTTP/1.0\r\nHost: x\r\nConnection: close\r\n\r\n");
    copy_bytes(request, "GET ", 4);
    copy_bytes(request + 4, path, path_len);
    copy_bytes(request + 4 + path_len, " HTTP/1.0\r\nHost: x\r\nConnection: close\r\n\r\n",
              str_len(" HTTP/1.0\r\nHost: x\r\nConnection: close\r\n\r\n"));
    request[req_len] = 0;
    net_log("https request sent");

    if (tls_write(conn, (const uint8_t *)request, (uint32_t)req_len) != 0) {
        tls_close(conn);
        net_error = NET_ERR_TLS_HANDSHAKE;
        return -1;
    }

    {
        uint64_t deadline = sched_ticks() + 1000;
        while (sched_ticks() < deadline) {
            uint8_t buf[TLS_CAP];
            uint32_t got = 0;
            if (tls_read(conn, buf, TLS_CAP, &got) == 0 && got > 0) {
                if (rx_size + got > NET_HTTP_CAP) {
                    tls_close(conn);
                    net_error = NET_ERR_HTTP_TOO_LARGE;
                    return -1;
                }
                copy_bytes(rx_buf + rx_size, buf, got);
                rx_size += got;
                deadline = sched_ticks() + 200;
            }
            sched_sleep(1);
        }
    }

    tls_close(conn);

    if (rx_size == 0) {
        net_error = NET_ERR_TCP_TIMEOUT;
        return -1;
    }

    header_end = find_header_end(rx_buf, rx_size);
    if (header_end < 0) {
        net_error = NET_ERR_HTTP_PARSE;
        return -1;
    }

    status = http_status_code(rx_buf, rx_size);
    if (status < 200 || status >= 300) {
        net_error = (uint32_t)(2000 + (status < 0 ? 0 : status));
        return -1;
    }

    if (vfs_write(vfs_root(), out_path, (const char *)(rx_buf + header_end), rx_size - (uint64_t)header_end) != 0) {
        net_error = NET_ERR_WRITE_FAILED;
        return -1;
    }

    if (bytes_out) *bytes_out = rx_size - (uint64_t)header_end;
    net_error = 0;
    return 0;
}
