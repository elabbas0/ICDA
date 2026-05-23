#ifndef ICDA_NET_H
#define ICDA_NET_H

#include <stdint.h>

#include "../memory/heap.h"
#include "../drivers/net/e1000.h"
#include "../drivers/console/console.h"
#include "../fs/vfs.h"
#include "../proc/sched.h"

// network error codes
#define NET_ERR_NO_NIC             1U
#define NET_ERR_ARP_TIMEOUT        2U
#define NET_ERR_TCP_TIMEOUT        3U
#define NET_ERR_TCP_REFUSED        4U
#define NET_ERR_HTTP_PARSE         5U
#define NET_ERR_HTTP_TOO_LARGE     6U
#define NET_ERR_WRITE_FAILED       7U
#define NET_ERR_URL_PATH           8U
#define NET_ERR_DNS_TIMEOUT        9U
#define NET_ERR_DNS_PARSE          10U
#define NET_ERR_TLS_HANDSHAKE      11U
#define NET_ERR_TLS_RECV           12U

// ethernet types
#define ETH_TYPE_ARP               0x0806U
#define ETH_TYPE_IPV4              0x0800U

// ARP constants
#define ARP_HTYPE_ETHERNET         1U
#define ARP_PTYPE_IPV4             0x0800U
#define ARP_OP_REQUEST             1U
#define ARP_OP_REPLY               2U

// IP protocol numbers
#define IP_PROTO_TCP               6U
#define IP_PROTO_UDP               17U

// TCP flags
#define TCP_FLAG_FIN               0x01U
#define TCP_FLAG_SYN               0x02U
#define TCP_FLAG_RST               0x04U
#define TCP_FLAG_PSH               0x08U
#define TCP_FLAG_ACK               0x10U

#define NET_FRAME_CAP              1600U
#define NET_HTTP_CAP               (8U * 1024U * 1024U)

#define NET_LOCAL_IP               0x0F02000AU // 10.0.2.15 little-endian host order
#define NET_GATEWAY_IP             0x0202000AU // 10.0.2.2
#define NET_DNS_IP                 0x01010101U // 1.1.1.1
#define NET_NETMASK                0x00FFFFFFU // 255.255.255.0

// packet structures
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
    uint16_t src_port_be;
    uint16_t dst_port_be;
    uint16_t len_be;
    uint16_t checksum_be;
} __attribute__((packed)) udp_hdr_t;

typedef struct {
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;
    const uint8_t *payload;
    uint16_t payload_len;
} tcp_packet_info_t;

typedef struct {
    const uint8_t *payload;
    uint16_t payload_len;
} udp_packet_info_t;

typedef struct {
    uint8_t mac[6];
    uint32_t ip;
    uint32_t gateway;
    uint32_t netmask;
    int ready;
} net_state_t;

// global net state
extern net_state_t net_state;

// byte order helpers
uint16_t bswap16(uint16_t value);
uint32_t bswap32(uint32_t value);
uint16_t htons16(uint16_t value);
uint16_t ntohs16(uint16_t value);
uint32_t htonl32(uint32_t value);
uint32_t ntohl32(uint32_t value);

// memory helpers
void copy_bytes(void *dst, const void *src, uint64_t size);
void zero_bytes(void *dst, uint64_t size);

// string helpers
uint64_t str_len(const char *text);
int str_prefix(const char *text, const char *prefix);

// checksum
uint32_t ip_checksum(const void *data, uint64_t size);
uint16_t tcp_checksum(const ipv4_hdr_t *ip, const tcp_hdr_t *tcp, const uint8_t *payload, uint16_t payload_len);

// IP helpers
int ip_same_subnet(uint32_t a, uint32_t b, uint32_t mask);
void ip_to_bytes(uint32_t ip, uint8_t out[4]);

// low-level helpers
void build_eth(eth_hdr_t *eth, const uint8_t dst[6], uint16_t type);

// TCP/UDP send
int send_tcp_packet(const uint8_t dst_mac[6], uint32_t dst_ip,
                    uint16_t src_port, uint16_t dst_port,
                    uint32_t seq, uint32_t ack,
                    uint8_t flags, const uint8_t *payload, uint16_t payload_len);
int send_udp_packet(const uint8_t dst_mac[6], uint32_t dst_ip,
                    uint16_t src_port, uint16_t dst_port,
                    const uint8_t *payload, uint16_t payload_len);

// TCP/UDP parse
int parse_tcp_packet(const uint8_t *frame, uint16_t len,
                     uint32_t expect_src_ip, uint16_t expect_src_port,
                     uint16_t expect_dst_port, tcp_packet_info_t *out);
int parse_udp_packet(const uint8_t *frame, uint16_t len,
                     uint32_t expect_src_ip, uint16_t expect_src_port,
                     uint16_t expect_dst_port, udp_packet_info_t *out);

// TCP connect
int tcp_connect(const uint8_t dst_mac[6], uint32_t dst_ip, uint16_t dst_port,
                uint16_t src_port, uint32_t *seq_io, uint32_t *ack_io);

// ARP
int net_arp_resolve(uint32_t target_ip, uint8_t mac_out[6]);

// HTTP helpers
int find_header_end(const uint8_t *buf, uint64_t size);
int http_status_code(const uint8_t *buf, uint64_t size);

// public API
int net_init(void);
int net_ready(void);
uint32_t net_last_error(void);
int net_dns_resolve_ipv4(const char *host, uint32_t *ipv4_out);
int net_http_get_ipv4(uint32_t ipv4_addr, uint16_t port, const char *path, const char *out_path, uint64_t *bytes_out);
int net_https_get_ipv4(uint32_t ipv4_addr, uint16_t port, const char *path, const char *out_path, uint64_t *bytes_out);

#endif
