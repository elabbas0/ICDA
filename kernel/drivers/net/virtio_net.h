#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>

/* Virtio PCI Device IDs */
#define VIRTIO_PCI_NETWORK_CARD  0x1000

/* Virtio Feature Bits (network device) */
#define VIRTIO_NET_F_MAC         (1 << 5)
#define VIRTIO_NET_F_STATUS      (1 << 16)

/* Virtio Queue Sizes */
#define VIRTIO_NET_QUEUE_SIZE    256

/* Virtio Net Header */
typedef struct {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    // uint16_t num_buffers; // Only if VIRTIO_NET_F_MRG_RXBUF
} __attribute__((packed)) virtio_net_hdr_t;

/* Virtio Descriptor */
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtio_net_desc_t;

/* Virtio Available Ring */
typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_NET_QUEUE_SIZE];
} __attribute__((packed)) virtio_net_avail_t;

/* Virtio Used Ring Element */
typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtio_net_used_elem_t;

/* Virtio Used Ring */
typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtio_net_used_elem_t ring[VIRTIO_NET_QUEUE_SIZE];
} __attribute__((packed)) virtio_net_used_t;

int virtio_net_init(void);
int virtio_net_ready(void);
uint32_t virtio_net_last_error(void);
int virtio_net_mac(uint8_t out[6]);
int virtio_net_send_frame(const void *data, uint16_t len);
int virtio_net_recv_frame(void *data, uint16_t cap, uint16_t *len_out);

#endif
