#include "e1000.h"

#include "../pci/pci.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"

#include <stdint.h>

#define E1000_VENDOR_ID            0x8086
#define E1000_DEVICE_82540EM       0x100E
#define E1000_DEVICE_82545EM       0x100F
#define E1000_DEVICE_82543GC       0x1004

#define E1000_REG_CTRL             0x0000
#define E1000_REG_STATUS           0x0008
#define E1000_REG_EERD             0x0014
#define E1000_REG_ICR              0x00C0
#define E1000_REG_IMS              0x00D0
#define E1000_REG_IMC              0x00D8
#define E1000_REG_RCTL             0x0100
#define E1000_REG_TCTL             0x0400
#define E1000_REG_TIPG             0x0410
#define E1000_REG_RDBAL            0x2800
#define E1000_REG_RDBAH            0x2804
#define E1000_REG_RDLEN            0x2808
#define E1000_REG_RDH              0x2810
#define E1000_REG_RDT              0x2818
#define E1000_REG_TDBAL            0x3800
#define E1000_REG_TDBAH            0x3804
#define E1000_REG_TDLEN            0x3808
#define E1000_REG_TDH              0x3810
#define E1000_REG_TDT              0x3818
#define E1000_REG_RAL0             0x5400
#define E1000_REG_RAH0             0x5404

#define E1000_RAH_AV               0x80000000U

#define E1000_CTRL_FD              0x00000001U
#define E1000_CTRL_ASDE            0x00000020U
#define E1000_CTRL_SLU             0x00000040U
#define E1000_CTRL_RST             0x04000000U

#define E1000_RCTL_EN              0x00000002U
#define E1000_RCTL_BAM             0x00008000U
#define E1000_RCTL_SECRC           0x04000000U

#define E1000_TCTL_EN              0x00000002U
#define E1000_TCTL_PSP             0x00000008U

#define E1000_TX_CMD_EOP           0x01U
#define E1000_TX_CMD_IFCS          0x02U
#define E1000_TX_CMD_RS            0x08U

#define E1000_TX_STATUS_DD         0x01U
#define E1000_RX_STATUS_DD         0x01U
#define E1000_RX_STATUS_EOP        0x02U

#define E1000_RING_COUNT           16U
#define E1000_BUF_SIZE             2048U

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    const pci_device_t *pci;
    volatile uint8_t *mmio;
    uint8_t mac[6];

    uint64_t rx_desc_phys;
    e1000_rx_desc_t *rx_desc;
    uint64_t tx_desc_phys;
    e1000_tx_desc_t *tx_desc;

    uint64_t rx_buf_phys[E1000_RING_COUNT];
    uint8_t *rx_buf[E1000_RING_COUNT];
    uint64_t tx_buf_phys[E1000_RING_COUNT];
    uint8_t *tx_buf[E1000_RING_COUNT];

    uint32_t rx_next;
    uint32_t tx_next;
    int ready;
} e1000_state_t;

static e1000_state_t e1000_state;
static uint32_t e1000_error = 0;

static inline void e1000_pause(void) {
    for (volatile uint32_t i = 0; i < 10000; i++) {
        __asm__ volatile("" ::: "memory");
    }
}

static inline uint32_t mmio_read32(uint32_t reg) {
    volatile uint32_t *ptr = (volatile uint32_t *)(e1000_state.mmio + reg);
    return *ptr;
}

static inline void mmio_write32(uint32_t reg, uint32_t value) {
    volatile uint32_t *ptr = (volatile uint32_t *)(e1000_state.mmio + reg);
    *ptr = value;
}

static void copy_bytes(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

static void zero_bytes(void *dst, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < size; i++) {
        d[i] = 0;
    }
}

static int e1000_mac_is_zero(const uint8_t mac[6]) {
    uint8_t acc = 0;
    for (uint32_t i = 0; i < 6; i++) {
        acc |= mac[i];
    }
    return acc == 0;
}

static int e1000_alloc_page(uint64_t *phys_out, uint8_t **virt_out) {
    uint64_t phys;
    if (!phys_out || !virt_out) return -1;
    phys = pmm_alloc_contiguous_below(1, 0xFFFFFFFFULL);
    if (!phys) return -1;
    *phys_out = phys;
    *virt_out = (uint8_t *)PHYS_TO_VIRT(phys);
    zero_bytes(*virt_out, PAGE_SIZE);
    return 0;
}

static int e1000_eeprom_read(uint8_t addr, uint16_t *value_out) {
    uint32_t cmd;
    uint32_t value;
    uint32_t spin = 0;
    if (!value_out) return -1;
    cmd = 1U | ((uint32_t)addr << 8);
    mmio_write32(E1000_REG_EERD, cmd);
    while (spin++ < 100000U) {
        value = mmio_read32(E1000_REG_EERD);
        if (value & (1U << 4)) {
            *value_out = (uint16_t)(value >> 16);
            return 0;
        }
    }
    return -1;
}

static int e1000_read_mac(void) {
    uint32_t ral = mmio_read32(E1000_REG_RAL0);
    uint32_t rah = mmio_read32(E1000_REG_RAH0);

    if ((rah & E1000_RAH_AV) != 0 || ral != 0 || (rah & 0x0000FFFFU) != 0) {
        e1000_state.mac[0] = (uint8_t)(ral & 0xFFU);
        e1000_state.mac[1] = (uint8_t)((ral >> 8) & 0xFFU);
        e1000_state.mac[2] = (uint8_t)((ral >> 16) & 0xFFU);
        e1000_state.mac[3] = (uint8_t)((ral >> 24) & 0xFFU);
        e1000_state.mac[4] = (uint8_t)(rah & 0xFFU);
        e1000_state.mac[5] = (uint8_t)((rah >> 8) & 0xFFU);
        if (!e1000_mac_is_zero(e1000_state.mac)) {
            return 0;
        }
    }

    {
        uint16_t word0;
        uint16_t word1;
        uint16_t word2;
        if (e1000_eeprom_read(0, &word0) != 0) return -1;
        if (e1000_eeprom_read(1, &word1) != 0) return -1;
        if (e1000_eeprom_read(2, &word2) != 0) return -1;
        e1000_state.mac[0] = (uint8_t)(word0 & 0xFFU);
        e1000_state.mac[1] = (uint8_t)((word0 >> 8) & 0xFFU);
        e1000_state.mac[2] = (uint8_t)(word1 & 0xFFU);
        e1000_state.mac[3] = (uint8_t)((word1 >> 8) & 0xFFU);
        e1000_state.mac[4] = (uint8_t)(word2 & 0xFFU);
        e1000_state.mac[5] = (uint8_t)((word2 >> 8) & 0xFFU);
        return 0;
    }
}

static void e1000_program_mac(void) {
    uint32_t ral = (uint32_t)e1000_state.mac[0] |
                   ((uint32_t)e1000_state.mac[1] << 8) |
                   ((uint32_t)e1000_state.mac[2] << 16) |
                   ((uint32_t)e1000_state.mac[3] << 24);
    uint32_t rah = (uint32_t)e1000_state.mac[4] |
                   ((uint32_t)e1000_state.mac[5] << 8) |
                   E1000_RAH_AV;

    mmio_write32(E1000_REG_RAL0, ral);
    mmio_write32(E1000_REG_RAH0, rah);
}

static int e1000_alloc_rings(void) {
    uint8_t *virt = 0;
    if (e1000_alloc_page(&e1000_state.rx_desc_phys, &virt) != 0) return -1;
    e1000_state.rx_desc = (e1000_rx_desc_t *)virt;
    if (e1000_alloc_page(&e1000_state.tx_desc_phys, &virt) != 0) return -1;
    e1000_state.tx_desc = (e1000_tx_desc_t *)virt;

    for (uint32_t i = 0; i < E1000_RING_COUNT; i++) {
        if (e1000_alloc_page(&e1000_state.rx_buf_phys[i], &e1000_state.rx_buf[i]) != 0) return -1;
        if (e1000_alloc_page(&e1000_state.tx_buf_phys[i], &e1000_state.tx_buf[i]) != 0) return -1;
    }
    return 0;
}

static void e1000_init_rings(void) {
    zero_bytes(e1000_state.rx_desc, PAGE_SIZE);
    zero_bytes(e1000_state.tx_desc, PAGE_SIZE);

    for (uint32_t i = 0; i < E1000_RING_COUNT; i++) {
        e1000_state.rx_desc[i].addr = e1000_state.rx_buf_phys[i];
        e1000_state.rx_desc[i].status = 0;
        e1000_state.tx_desc[i].addr = e1000_state.tx_buf_phys[i];
        e1000_state.tx_desc[i].status = E1000_TX_STATUS_DD;
    }

    mmio_write32(E1000_REG_RDBAL, (uint32_t)e1000_state.rx_desc_phys);
    mmio_write32(E1000_REG_RDBAH, (uint32_t)(e1000_state.rx_desc_phys >> 32));
    mmio_write32(E1000_REG_RDLEN, E1000_RING_COUNT * sizeof(e1000_rx_desc_t));
    mmio_write32(E1000_REG_RDH, 0);
    mmio_write32(E1000_REG_RDT, E1000_RING_COUNT - 1);

    mmio_write32(E1000_REG_TDBAL, (uint32_t)e1000_state.tx_desc_phys);
    mmio_write32(E1000_REG_TDBAH, (uint32_t)(e1000_state.tx_desc_phys >> 32));
    mmio_write32(E1000_REG_TDLEN, E1000_RING_COUNT * sizeof(e1000_tx_desc_t));
    mmio_write32(E1000_REG_TDH, 0);
    mmio_write32(E1000_REG_TDT, 0);

    e1000_state.rx_next = 0;
    e1000_state.tx_next = 0;
}

static void e1000_enable_io(void) {
    mmio_write32(E1000_REG_IMC, 0xFFFFFFFFU);
    (void)mmio_read32(E1000_REG_ICR);
    mmio_write32(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    mmio_write32(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (0x10U << 4) | (0x40U << 12));
    mmio_write32(E1000_REG_TIPG, 0x0060200AU);
}

int e1000_init(void) {
    const pci_device_t *pci = 0;
    uint32_t bar0;
    uint64_t mmio_phys;

    zero_bytes(&e1000_state, sizeof(e1000_state));
    e1000_error = 0;

    for (uint32_t i = 0; i < pci_device_count(); i++) {
        const pci_device_t *cand = pci_device_at(i);
        if (!cand) continue;
        if (cand->vendor_id != E1000_VENDOR_ID) continue;
        if (cand->device_id == E1000_DEVICE_82540EM ||
            cand->device_id == E1000_DEVICE_82545EM ||
            cand->device_id == E1000_DEVICE_82543GC) {
            pci = cand;
            break;
        }
    }

    if (!pci) {
        e1000_error = 1;
        return -1;
    }

    if (pci_enable_memory_busmaster(pci) != 0) {
        e1000_error = 2;
        return -1;
    }

    bar0 = pci_read_config32(pci, 0x10);
    if ((bar0 & 0x1U) != 0) {
        e1000_error = 3;
        return -1;
    }
    mmio_phys = (uint64_t)(bar0 & ~0xFU);
    if (!mmio_phys) {
        e1000_error = 4;
        return -1;
    }

    e1000_state.mmio = (volatile uint8_t *)vmm_map_physical(mmio_phys, 0x20000U, VMM_FLAGS_KERNEL_RW);
    if (!e1000_state.mmio) {
        e1000_error = 5;
        return -1;
    }
    e1000_state.pci = pci;

    mmio_write32(E1000_REG_CTRL, mmio_read32(E1000_REG_CTRL) | E1000_CTRL_RST);
    e1000_pause();
    mmio_write32(E1000_REG_CTRL, mmio_read32(E1000_REG_CTRL) | E1000_CTRL_FD | E1000_CTRL_ASDE | E1000_CTRL_SLU);
    e1000_pause();

    if (e1000_alloc_rings() != 0) {
        e1000_error = 6;
        return -1;
    }
    if (e1000_read_mac() != 0) {
        e1000_error = 7;
        return -1;
    }

    e1000_program_mac();
    e1000_init_rings();
    e1000_enable_io();
    e1000_state.ready = 1;
    return 0;
}

int e1000_ready(void) {
    return e1000_state.ready;
}

uint32_t e1000_last_error(void) {
    return e1000_error;
}

int e1000_mac(uint8_t out[6]) {
    if (!e1000_state.ready || !out) return -1;
    for (uint32_t i = 0; i < 6; i++) out[i] = e1000_state.mac[i];
    return 0;
}

int e1000_send_frame(const void *data, uint16_t len) {
    uint32_t idx;
    e1000_tx_desc_t *desc;
    uint32_t spin = 0;

    if (!e1000_state.ready || !data || len == 0 || len > E1000_BUF_SIZE) return -1;

    idx = e1000_state.tx_next;
    desc = &e1000_state.tx_desc[idx];
    while (!(desc->status & E1000_TX_STATUS_DD)) {
        if (++spin > 1000000U) return -1;
    }

    copy_bytes(e1000_state.tx_buf[idx], data, len);
    desc->length = len;
    desc->cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    __asm__ volatile("" ::: "memory");

    e1000_state.tx_next = (idx + 1U) % E1000_RING_COUNT;
    mmio_write32(E1000_REG_TDT, e1000_state.tx_next);
    (void)mmio_read32(E1000_REG_STATUS);
    return 0;
}

int e1000_recv_frame(void *data, uint16_t cap, uint16_t *len_out) {
    uint32_t idx;
    e1000_rx_desc_t *desc;
    uint16_t len;

    if (!e1000_state.ready || !data || !len_out) return -1;

    idx = e1000_state.rx_next;
    desc = &e1000_state.rx_desc[idx];
    if (!(desc->status & E1000_RX_STATUS_DD) || !(desc->status & E1000_RX_STATUS_EOP)) {
        return 0;
    }

    len = desc->length;
    if (len > cap) len = cap;
    copy_bytes(data, e1000_state.rx_buf[idx], len);
    *len_out = len;

    desc->status = 0;
    mmio_write32(E1000_REG_RDT, idx);
    e1000_state.rx_next = (idx + 1U) % E1000_RING_COUNT;
    return 1;
}
