/*
 * virtio-net.c — Virtio Network Device Driver
 *
 * Supports virtio-net-pci for VirtualBox (paravirtualized NIC),
 * QEMU virtio, and real hardware with virtio support.
 * Falls back gracefully if no virtio device is found.
 */
#include "virtio_net.h"
#include "../pci/pci.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"
#include "../../proc/sched.h"
#include <stdint.h>

/* Virtio PCI capability offsets */
#define VIRTIO_PCI_CAP_VENDOR       0x09
#define VIRTIO_PCI_CFG_CAP_OFFSET   0x34

/* Virtio register offsets (modern, capability-based) */
#define VIRTIO_REG_DEVICE_FEATURES  0x00
#define VIRTIO_REG_DRIVER_FEATURES  0x04
#define VIRTIO_REG_QUEUE_SIZE        0x0C
#define VIRTIO_REG_QUEUE_SELECT      0x0E
#define VIRTIO_REG_QUEUE_NOTIFY     0x10
#define VIRTIO_REG_DEVICE_STATUS    0x12
#define VIRTIO_REG_ISR_STATUS       0x13

/* Status bits */
#define VIRTIO_STATUS_ACK           0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FAILED        0x80

/* Descriptor flags */
#define VIRTIO_DESC_F_NEXT          0x01
#define VIRTIO_DESC_F_WRITE         0x02
#define VIRTIO_DESC_F_INDIRECT      0x04

/* Feature bits */
#define VIRTIO_F_NOTIFY_ON_EMPTY    (1 << 24)

typedef struct {
    const pci_device_t *pci;
    volatile uint8_t *mmio;

    uint8_t mac[6];
    uint16_t status;

    /* TX queue (queue 0) */
    virtio_net_desc_t  *tx_desc;
    virtio_net_avail_t *tx_avail;
    virtio_net_used_t  *tx_used;
    uint64_t tx_desc_phys;
    uint64_t tx_avail_phys;
    uint64_t tx_used_phys;
    uint8_t *tx_bufs[VIRTIO_NET_QUEUE_SIZE];
    uint64_t tx_buf_phys[VIRTIO_NET_QUEUE_SIZE];
    uint32_t tx_next_desc;
    uint32_t tx_last_used;

    /* RX queue (queue 1) */
    virtio_net_desc_t  *rx_desc;
    virtio_net_avail_t *rx_avail;
    virtio_net_used_t  *rx_used;
    uint64_t rx_desc_phys;
    uint64_t rx_avail_phys;
    uint64_t rx_used_phys;
    uint8_t *rx_bufs[VIRTIO_NET_QUEUE_SIZE];
    uint64_t rx_buf_phys[VIRTIO_NET_QUEUE_SIZE];
    uint32_t rx_next_free;
    uint32_t rx_last_used;

    volatile uint8_t *cfg;  /* MMIO base for device-specific config */

    int ready;
} virtio_net_state_t;

static virtio_net_state_t vn;
static uint32_t vn_error = 0;

static inline uint8_t vn_read8(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint8_t *)((uint64_t)base + off);
}

static inline void vn_write8(volatile uint8_t *base, uint32_t off, uint8_t val) {
    *(volatile uint8_t *)((uint64_t)base + off) = val;
}

static inline uint16_t vn_read16(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint16_t *)((uint64_t)base + off);
}

static inline void vn_write16(volatile uint8_t *base, uint32_t off, uint16_t val) {
    *(volatile uint16_t *)((uint64_t)base + off) = val;
}

static inline uint32_t vn_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)((uint64_t)base + off);
}

static inline void vn_write32(volatile uint8_t *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)((uint64_t)base + off) = val;
}

static void vn_pause(void) {
    for (volatile uint32_t i = 0; i < 5000; i++) {
        __asm__ volatile("" ::: "memory");
    }
}

static int vn_alloc_page(uint64_t *phys_out, void **virt_out) {
    uint64_t phys = pmm_alloc_contiguous_below(1, 0xFFFFFFFFULL);
    if (!phys) return -1;
    *phys_out = phys;
    *virt_out = (void *)PHYS_TO_VIRT(phys);
    /* Zero the page */
    uint8_t *v = (uint8_t *)*virt_out;
    for (uint32_t i = 0; i < 4096; i++) v[i] = 0;
    return 0;
}

/*
 * Scan the Virtio PCI capability list for a modern (virtio 1.0) device.
 * Returns the MMIO base for a given capability type, or 0 if not found.
 * cap_type: 1 = common config, 4 = device config
 */
static volatile uint8_t *vn_find_pci_cap(const pci_device_t *pci, uint8_t cap_type) {
    uint8_t cap_off = pci_read_config8(pci, VIRTIO_PCI_CFG_CAP_OFFSET);
    serial_write("[virtio] cap_ptr=");
    { char _b[3]; _b[0]="0123456789abcdef"[(cap_off>>4)&0xF]; _b[1]="0123456789abcdef"[cap_off&0xF]; _b[2]=0; serial_write(_b); }
    serial_write("\n");
    if (cap_off == 0) return 0;
    for (int i = 0; i < 32 && cap_off != 0; i++) {
        uint8_t cap_id = pci_read_config8(pci, cap_off);
        uint8_t cap_next = pci_read_config8(pci, cap_off + 1);
        if (cap_id == 0x09) {
            uint8_t cfg_type = pci_read_config8(pci, cap_off + 3);
            uint8_t bar = pci_read_config8(pci, cap_off + 4);
            uint32_t bar_off = pci_read_config32(pci, cap_off + 8);
            serial_write("[virtio] cap type=");
            { char _b[3]; _b[0]="0123456789abcdef"[(cfg_type>>4)&0xF]; _b[1]="0123456789abcdef"[cfg_type&0xF]; _b[2]=0; serial_write(_b); }
            serial_write(" bar=");
            { char _b[3]; _b[0]="0123456789abcdef"[(bar>>4)&0xF]; _b[1]="0123456789abcdef"[bar&0xF]; _b[2]=0; serial_write(_b); }
            serial_write(" off=");
            { char _b[9]; uint32_t _v=bar_off; for(int _i=7;_i>=0;_i--){_b[7-_i]="0123456789abcdef"[(_v>>(_i*4))&0xF];} _b[8]=0; serial_write(_b); }
            serial_write("\n");
            if (cfg_type == cap_type && bar <= 5) {
                uint32_t bar_val = pci_read_config32(pci, 0x10 + bar * 4);
                serial_write("[virtio] bar_val=");
                { char _b[9]; uint32_t _v=bar_val; for(int _i=7;_i>=0;_i--){_b[7-_i]="0123456789abcdef"[(_v>>(_i*4))&0xF];} _b[8]=0; serial_write(_b); }
                serial_write("\n");
                if (bar_val == 0) { serial_write("[virtio] bar==0 skip\n"); }
                else if (bar_val & 0x01) { serial_write("[virtio] I/O bar skip\n"); }
                else {
                    uint64_t mmio_phys = (uint64_t)(bar_val & ~0xFU) + bar_off;
                    if (mmio_phys == 0) return 0;
                    serial_write("[virtio] mapping...");
                    volatile uint8_t *p = (volatile uint8_t *)vmm_map_physical(mmio_phys, 0x1000, VMM_FLAGS_KERNEL_RW);
                    serial_write(p ? "OK\n" : "FAIL\n");
                    return p;
                }
            }
        }
        cap_off = cap_next & 0xFC;
    }
    return 0;
}

/*
 * Legacy (transitional) virtio: MMIO at BAR0 offset 0x20.
 */
static volatile uint8_t *vn_legacy_mmio(const pci_device_t *pci) {
    uint32_t bar0 = pci_read_config32(pci, 0x10);
    if (bar0 == 0) return 0;
    if (bar0 & 0x01) return 0; /* I/O BAR */
    uint64_t mmio_phys = (uint64_t)(bar0 & ~0xFU);
    if (mmio_phys == 0) return 0;
    return (volatile uint8_t *)vmm_map_physical(mmio_phys, 0x1000, VMM_FLAGS_KERNEL_RW);
}

static int vn_init_queues(void) {
    /* TX queue (queue 0) */
    if (vn_alloc_page(&vn.tx_desc_phys, (void **)&vn.tx_desc) != 0) return -1;
    if (vn_alloc_page(&vn.tx_avail_phys, (void **)&vn.tx_avail) != 0) return -1;
    if (vn_alloc_page(&vn.tx_used_phys, (void **)&vn.tx_used) != 0) return -1;

    for (uint32_t i = 0; i < VIRTIO_NET_QUEUE_SIZE; i++) {
        if (vn_alloc_page(&vn.tx_buf_phys[i], (void **)&vn.tx_bufs[i]) != 0) return -1;
        vn.tx_desc[i].addr = vn.tx_buf_phys[i];
        vn.tx_desc[i].len = 0;
        vn.tx_desc[i].flags = VIRTIO_DESC_F_WRITE;  /* host reads */
        vn.tx_desc[i].next = 0;
    }
    vn.tx_next_desc = 0;
    vn.tx_last_used = 0;
    vn.tx_avail->idx = 0;

    /* RX queue (queue 1) */
    if (vn_alloc_page(&vn.rx_desc_phys, (void **)&vn.rx_desc) != 0) return -1;
    if (vn_alloc_page(&vn.rx_avail_phys, (void **)&vn.rx_avail) != 0) return -1;
    if (vn_alloc_page(&vn.rx_used_phys, (void **)&vn.rx_used) != 0) return -1;

    for (uint32_t i = 0; i < VIRTIO_NET_QUEUE_SIZE; i++) {
        if (vn_alloc_page(&vn.rx_buf_phys[i], (void **)&vn.rx_bufs[i]) != 0) return -1;
        vn.rx_desc[i].addr = vn.rx_buf_phys[i];
        vn.rx_desc[i].len = 4096;
        vn.rx_desc[i].flags = VIRTIO_DESC_F_WRITE;
        vn.rx_desc[i].next = 0;
        /* Pre-fill the available ring */
        vn.rx_avail->ring[i] = i;
    }
    vn.rx_next_free = 0;
    vn.rx_last_used = 0;
    vn.rx_avail->idx = VIRTIO_NET_QUEUE_SIZE;

    return 0;
}

static void vn_select_queue(uint16_t q) {
    vn_write16(vn.mmio, VIRTIO_REG_QUEUE_SELECT, q);
    vn_pause();
}

static void vn_notify_queue(uint16_t q) {
    vn_write16(vn.mmio, VIRTIO_REG_QUEUE_NOTIFY, q);
    vn_pause();
}

int virtio_net_init(void) {
    const pci_device_t *pci = 0;

    serial_write("[virtio] scanning PCI...\n");
    for (uint32_t i = 0; i < pci_device_count(); i++) {
        const pci_device_t *cand = pci_device_at(i);
        if (!cand) continue;
        if (cand->vendor_id != 0x1AF4) continue;
        if (cand->device_id >= 0x1000 && cand->device_id <= 0x103F) { pci = cand; break; }
        if (cand->device_id == 0x1041) { pci = cand; break; }
    }

    if (!pci) {
        serial_write("[virtio] no device found\n");
        vn_error = 1;
        return -1;
    }
    serial_write("[virtio] found device\n");

    if (pci_enable_memory_busmaster(pci) != 0) {
        serial_write("[virtio] busmaster failed\n");
        vn_error = 2;
        return -1;
    }
    serial_write("[virtio] busmaster OK\n");

    /* Read BAR0 value */
    uint32_t bar0_val = pci_read_config32(pci, 0x10);
    serial_write("[virtio] BAR0=");
    { char _b[9]; uint32_t _v=bar0_val; for(int _i=7;_i>=0;_i--){_b[7-_i]="0123456789abcdef"[(_v>>(_i*4))&0xF];} _b[8]=0; serial_write(_b); }
    serial_write("\n");

    vn.mmio = vn_legacy_mmio(pci);
    serial_write(vn.mmio ? "[virtio] legacy MMIO mapped\n" : "[virtio] legacy MMIO failed\n");

    if (!vn.mmio) {
        serial_write("[virtio] trying modern caps...\n");
        vn.mmio = vn_find_pci_cap(pci, 1);
        vn.cfg = vn_find_pci_cap(pci, 4);
        serial_write(vn.mmio ? "[virtio] modern MMIO mapped\n" : "[virtio] modern MMIO failed\n");
    }

    if (!vn.mmio) {
        vn_error = 3;
        return -1;
    }

    vn.pci = pci;
    serial_write("[virtio] MMIO ready\n");

    serial_write("[virtio] reset\n");
    vn_write8(vn.mmio, VIRTIO_REG_DEVICE_STATUS, 0);
    vn_pause();

    serial_write("[virtio] ack\n");
    vn_write8(vn.mmio, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    vn_pause();

    serial_write("[virtio] driver\n");
    vn_write8(vn.mmio, VIRTIO_REG_DEVICE_STATUS,
              VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    vn_pause();

    serial_write("[virtio] read features\n");
    uint32_t host_features = vn_read32(vn.mmio, VIRTIO_REG_DEVICE_FEATURES);
    serial_write("[virtio] features ok\n");
    uint32_t wanted = 0;
    if (host_features & VIRTIO_NET_F_MAC) wanted |= VIRTIO_NET_F_MAC;
    if (host_features & VIRTIO_NET_F_STATUS) wanted |= VIRTIO_NET_F_STATUS;
    vn_write32(vn.mmio, VIRTIO_REG_DRIVER_FEATURES, wanted);
    vn_pause();
    serial_write("[virtio] features negotiated\n");

    /* Setup queues */
    vn_select_queue(0); /* TX */
    uint16_t tx_size = vn_read16(vn.mmio, VIRTIO_REG_QUEUE_SIZE);
    vn_select_queue(1); /* RX */
    uint16_t rx_size = vn_read16(vn.mmio, VIRTIO_REG_QUEUE_SIZE);

    if (tx_size == 0 || rx_size == 0) {
        vn_error = 4;
        vn_write8(vn.mmio, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    if (vn_init_queues() != 0) {
        vn_error = 5;
        vn_write8(vn.mmio, VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -1;
    }

    /* Tell device about TX queue */
    vn_select_queue(0);
    vn_write32(vn.mmio, 0x08, (uint32_t)vn.tx_desc_phys);  /* Queue PFN (legacy) */
    vn_write32(vn.mmio, 0x0C, (uint32_t)(vn.tx_desc_phys >> 32));
    vn_pause();

    /* Tell device about RX queue */
    vn_select_queue(1);
    vn_write32(vn.mmio, 0x08, (uint32_t)vn.rx_desc_phys);
    vn_write32(vn.mmio, 0x0C, (uint32_t)(vn.rx_desc_phys >> 32));
    vn_pause();

    /* Read MAC address from device config */
    if (vn.cfg && (wanted & VIRTIO_NET_F_MAC)) {
        for (int i = 0; i < 6; i++) {
            vn.mac[i] = vn_read8(vn.cfg, i);
        }
    } else {
        /* Fallback: use a fixed MAC for testing */
        vn.mac[0] = 0x52; vn.mac[1] = 0x54;
        vn.mac[2] = 0x00; vn.mac[3] = 0x12;
        vn.mac[4] = 0x34; vn.mac[5] = 0x56;
    }

    /* Set driver OK */
    vn_write8(vn.mmio, VIRTIO_REG_DEVICE_STATUS,
              VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
    vn_pause();

    vn.ready = 1;
    return 0;
}

int virtio_net_ready(void) {
    return vn.ready;
}

uint32_t virtio_net_last_error(void) {
    return vn_error;
}

int virtio_net_mac(uint8_t out[6]) {
    if (!vn.ready || !out) return -1;
    for (int i = 0; i < 6; i++) out[i] = vn.mac[i];
    return 0;
}

int virtio_net_send_frame(const void *data, uint16_t len) {
    if (!vn.ready || !data || len == 0) return -1;

    uint32_t desc_idx = vn.tx_next_desc;
    uint16_t total_len = sizeof(virtio_net_hdr_t) + len;

    if (total_len > 4096) return -1;

    /* Prepend virtio-net header */
    virtio_net_hdr_t *hdr = (virtio_net_hdr_t *)vn.tx_bufs[desc_idx];
    hdr->flags = 0;
    hdr->gso_type = 0;
    hdr->hdr_len = sizeof(virtio_net_hdr_t);
    hdr->gso_size = 0;
    hdr->csum_start = 0;
    hdr->csum_offset = 0;

    /* Copy frame data after header */
    uint8_t *dst = vn.tx_bufs[desc_idx] + sizeof(virtio_net_hdr_t);
    const uint8_t *src = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    /* Setup descriptor */
    vn.tx_desc[desc_idx].addr = vn.tx_buf_phys[desc_idx];
    vn.tx_desc[desc_idx].len = total_len;
    vn.tx_desc[desc_idx].flags = 0;  /* host reads */
    vn.tx_desc[desc_idx].next = 0;

    /* Add to available ring */
    uint16_t avail_idx = vn.tx_avail->idx;
    vn.tx_avail->ring[avail_idx % VIRTIO_NET_QUEUE_SIZE] = desc_idx;
    __asm__ volatile("" ::: "memory");
    vn.tx_avail->idx = avail_idx + 1;
    __asm__ volatile("" ::: "memory");

    /* Notify device */
    vn_notify_queue(0);

    /* Advance to next descriptor */
    vn.tx_next_desc = (desc_idx + 1) % VIRTIO_NET_QUEUE_SIZE;

    return 0;
}

int virtio_net_recv_frame(void *data, uint16_t cap, uint16_t *len_out) {
    if (!vn.ready || !data || !len_out) return -1;

    /* Check if there are new used buffers */
    uint16_t used_idx = vn.rx_used->idx;
    if (vn.rx_last_used == used_idx) return 0; /* No new data */

    __asm__ volatile("" ::: "memory");

    /* Process the oldest used buffer */
    uint16_t slot = vn.rx_last_used % VIRTIO_NET_QUEUE_SIZE;
    virtio_net_used_elem_t *elem = &vn.rx_used->ring[slot];
    uint32_t desc_id = elem->id;
    uint32_t recv_len = elem->len;

    /* Skip the virtio-net header */
    uint16_t data_offset = sizeof(virtio_net_hdr_t);
    uint16_t data_len = (recv_len > data_offset) ? (uint16_t)(recv_len - data_offset) : 0;

    if (data_len > cap) data_len = cap;

    /* Copy data from receive buffer */
    uint8_t *src = vn.rx_bufs[desc_id] + data_offset;
    uint8_t *dst = (uint8_t *)data;
    for (uint16_t i = 0; i < data_len; i++) dst[i] = src[i];

    *len_out = data_len;

    /* Re-arm the descriptor: reset it and put it back on the available ring */
    vn.rx_desc[desc_id].addr = vn.rx_buf_phys[desc_id];
    vn.rx_desc[desc_id].len = 4096;
    vn.rx_desc[desc_id].flags = VIRTIO_DESC_F_WRITE;

    uint16_t avail_idx = vn.rx_avail->idx;
    vn.rx_avail->ring[avail_idx % VIRTIO_NET_QUEUE_SIZE] = desc_id;
    __asm__ volatile("" ::: "memory");
    vn.rx_avail->idx = avail_idx + 1;
    __asm__ volatile("" ::: "memory");

    /* Notify device that buffers are available */
    vn_notify_queue(1);

    vn.rx_last_used = used_idx + 1;

    return 1;
}
