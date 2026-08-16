#include "nvme.h"

#include "../pci/pci.h"
#include "../serial/serial.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"

#include <stdint.h>

#define NVME_CLASS_STORAGE  0x01
#define NVME_SUBCLASS_NVM   0x08
#define NVME_PROGIF_NVME    0x02

#define NVME_MAX_DEVICES    8
#define NVME_ADMIN_Q_DEPTH  16
#define NVME_IO_Q_DEPTH     16

#define NVME_REG_CAP        0x0000
#define NVME_REG_VS         0x0008
#define NVME_REG_CC         0x0014
#define NVME_REG_CSTS       0x001C
#define NVME_REG_AQA        0x0024
#define NVME_REG_ASQ        0x0028
#define NVME_REG_ACQ        0x0030
#define NVME_REG_DBS        0x1000

#define NVME_CC_EN          (1U << 0)
#define NVME_CSTS_RDY       (1U << 0)

#define NVME_ADMIN_OP_DELETE_IO_SQ   0x00
#define NVME_ADMIN_OP_CREATE_IO_SQ   0x01
#define NVME_ADMIN_OP_GET_LOG_PAGE   0x02
#define NVME_ADMIN_OP_DELETE_IO_CQ   0x04
#define NVME_ADMIN_OP_CREATE_IO_CQ   0x05
#define NVME_ADMIN_OP_IDENTIFY       0x06
#define NVME_ADMIN_OP_SET_FEATURES   0x09

#define NVME_NVM_OP_WRITE            0x01
#define NVME_NVM_OP_READ             0x02

#define NVME_FEAT_NUM_QUEUES         0x07

typedef struct {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsv2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_cmd_t;

typedef struct {
    uint32_t cdw0;
    uint32_t rsv1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed)) nvme_cpl_t;

typedef struct {
    uint64_t nsze;
    uint64_t ncap;
    uint64_t nuse;
    uint8_t nsfeat;
    uint8_t nlbaf;
    uint8_t flbas;
    uint8_t mc;
    uint8_t dpc;
    uint8_t dps;
    uint8_t nmic;
    uint8_t rescap;
    uint8_t fpi;
    uint8_t dlfeat;
    uint16_t nawun;
    uint16_t nawupf;
    uint16_t nacwu;
    uint16_t nabsn;
    uint16_t nabo;
    uint16_t nabspf;
    uint16_t noiob;
    uint8_t rsv1[72];
    struct {
        uint16_t ms;
        uint8_t lbads;
        uint8_t rp;
    } __attribute__((packed)) lbaf[16];
} __attribute__((packed)) nvme_identify_ns_t;

typedef struct {
    int present;
    const pci_device_t *pci;
    volatile uint8_t *regs;
    uint32_t dstrd_bytes;
    uint16_t admin_cid;
    uint16_t io_cid;
    uint32_t admin_sq_tail;
    uint32_t admin_cq_head;
    uint32_t io_sq_tail;
    uint32_t io_cq_head;
    uint16_t admin_phase;
    uint16_t io_phase;
    uint64_t admin_sq_phys;
    uint64_t admin_cq_phys;
    uint64_t io_sq_phys;
    uint64_t io_cq_phys;
    uint64_t identify_phys;
    uint8_t *identify_virt;
    uint64_t dma_phys;
    uint8_t *dma_virt;
    uint32_t dma_bytes;
    uint32_t lba_size;
    uint64_t sector_count;
    uint32_t namespace_id;
    char name[8];
    block_device_t block;
} nvme_device_t;

static nvme_device_t nvme_devices[NVME_MAX_DEVICES];
static uint32_t nvme_count = 0;
static uint16_t nvme_last_status = 0;

static void nvme_trace(const char *msg) {
    serial_write("[nvme] ");
    serial_write(msg);
    serial_write("\n");
}

static void nvme_trace_hex16(uint16_t value) {
    char buf[7];
    static const char hex[] = "0123456789abcdef";
    buf[0] = '0';
    buf[1] = 'x';
    buf[2] = hex[(value >> 12) & 0xF];
    buf[3] = hex[(value >> 8) & 0xF];
    buf[4] = hex[(value >> 4) & 0xF];
    buf[5] = hex[value & 0xF];
    buf[6] = 0;
    serial_write("[nvme] status=");
    serial_write(buf);
    serial_write("\n");
}

static void mem_zero(uint8_t *dst, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) dst[i] = 0;
}

static inline uint32_t nvme_reg32(nvme_device_t *dev, uint32_t off) {
    return *(volatile uint32_t *)(dev->regs + off);
}

static inline void nvme_reg32_write(nvme_device_t *dev, uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(dev->regs + off) = value;
}

static inline uint64_t nvme_reg64(nvme_device_t *dev, uint32_t off) {
    return *(volatile uint64_t *)(dev->regs + off);
}

static inline void nvme_reg64_write(nvme_device_t *dev, uint32_t off, uint64_t value) {
    *(volatile uint64_t *)(dev->regs + off) = value;
}

static inline uint32_t *nvme_sq_db(nvme_device_t *dev, uint16_t qid) {
    return (uint32_t *)(dev->regs + NVME_REG_DBS + (2U * qid) * dev->dstrd_bytes);
}

static inline uint32_t *nvme_cq_db(nvme_device_t *dev, uint16_t qid) {
    return (uint32_t *)(dev->regs + NVME_REG_DBS + ((2U * qid) + 1U) * dev->dstrd_bytes);
}

static int nvme_wait_ready(nvme_device_t *dev, int ready, uint32_t limit) {
    while (limit--) {
        uint32_t csts = nvme_reg32(dev, NVME_REG_CSTS);
        if (((csts & NVME_CSTS_RDY) != 0) == ready) return 0;
    }
    return -1;
}

static int nvme_admin_submit(nvme_device_t *dev, nvme_cmd_t *cmd) {
    nvme_cmd_t *sq = (nvme_cmd_t *)PHYS_TO_VIRT(dev->admin_sq_phys);
    nvme_cpl_t *cq = (nvme_cpl_t *)PHYS_TO_VIRT(dev->admin_cq_phys);
    uint16_t cid = ++dev->admin_cid;
    uint32_t slot = dev->admin_sq_tail;
    uint32_t head = dev->admin_cq_head;
    uint16_t phase = dev->admin_phase;

    cmd->cid = cid;
    sq[slot] = *cmd;
    dev->admin_sq_tail = (slot + 1U) % NVME_ADMIN_Q_DEPTH;
    *nvme_sq_db(dev, 0) = dev->admin_sq_tail;

    for (uint32_t i = 0; i < 5000000U; i++) {
        nvme_cpl_t *cpl = &cq[head];
        if ((cpl->status & 1U) != phase) continue;
        if (cpl->cid != cid) continue;
        if ((cpl->status >> 1) != 0) {
            nvme_last_status = cpl->status;
            return -1;
        }
        dev->admin_cq_head = (head + 1U) % NVME_ADMIN_Q_DEPTH;
        if (dev->admin_cq_head == 0) dev->admin_phase ^= 1U;
        *nvme_cq_db(dev, 0) = dev->admin_cq_head;
        return 0;
    }
    return -1;
}

static int nvme_io_submit(nvme_device_t *dev, nvme_cmd_t *cmd) {
    nvme_cmd_t *sq = (nvme_cmd_t *)PHYS_TO_VIRT(dev->io_sq_phys);
    nvme_cpl_t *cq = (nvme_cpl_t *)PHYS_TO_VIRT(dev->io_cq_phys);
    uint16_t cid = ++dev->io_cid;
    uint32_t slot = dev->io_sq_tail;
    uint32_t head = dev->io_cq_head;
    uint16_t phase = dev->io_phase;

    cmd->cid = cid;
    sq[slot] = *cmd;
    dev->io_sq_tail = (slot + 1U) % NVME_IO_Q_DEPTH;
    *nvme_sq_db(dev, 1) = dev->io_sq_tail;

    for (uint32_t i = 0; i < 5000000U; i++) {
        nvme_cpl_t *cpl = &cq[head];
        if ((cpl->status & 1U) != phase) continue;
        if (cpl->cid != cid) continue;
        if ((cpl->status >> 1) != 0) {
            nvme_last_status = cpl->status;
            return -1;
        }
        dev->io_cq_head = (head + 1U) % NVME_IO_Q_DEPTH;
        if (dev->io_cq_head == 0) dev->io_phase ^= 1U;
        *nvme_cq_db(dev, 1) = dev->io_cq_head;
        return 0;
    }
    return -1;
}

static int nvme_identify_namespace(nvme_device_t *dev, uint32_t nsid, nvme_identify_ns_t *out) {
    nvme_cmd_t cmd;
    mem_zero((uint8_t *)&cmd, sizeof(cmd));
    mem_zero(dev->identify_virt, PAGE_SIZE);
    cmd.opcode = NVME_ADMIN_OP_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = dev->identify_phys;
    cmd.cdw10 = 0;
    if (nvme_admin_submit(dev, &cmd) != 0) return -1;
    *out = *(nvme_identify_ns_t *)dev->identify_virt;
    return 0;
}

static int nvme_setup_io_queues(nvme_device_t *dev) {
    nvme_cmd_t cmd;

    mem_zero((uint8_t *)&cmd, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_SET_FEATURES;
    cmd.cdw10 = NVME_FEAT_NUM_QUEUES;
    cmd.cdw11 = 0;
    if (nvme_admin_submit(dev, &cmd) != 0) {
        nvme_trace("set features num queues failed");
        nvme_trace_hex16(nvme_last_status);
        return -1;
    }

    mem_zero((uint8_t *)&cmd, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_CREATE_IO_CQ;
    cmd.prp1 = dev->io_cq_phys;
    cmd.cdw10 = (1U | ((NVME_IO_Q_DEPTH - 1U) << 16));
    cmd.cdw11 = 1U;
    if (nvme_admin_submit(dev, &cmd) != 0) {
        nvme_trace("create io cq failed");
        nvme_trace_hex16(nvme_last_status);
        return -1;
    }

    mem_zero((uint8_t *)&cmd, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_OP_CREATE_IO_SQ;
    cmd.prp1 = dev->io_sq_phys;
    cmd.cdw10 = (1U | ((NVME_IO_Q_DEPTH - 1U) << 16));
    cmd.cdw11 = (1U | (1U << 16));
    if (nvme_admin_submit(dev, &cmd) != 0) {
        nvme_trace("create io sq failed");
        nvme_trace_hex16(nvme_last_status);
        return -1;
    }

    return 0;
}

static int nvme_rw(nvme_device_t *dev, uint64_t lba, uint32_t count, void *buffer, int write) {
    uint8_t *ptr = (uint8_t *)buffer;
    uint32_t max_count;

    if (!dev || !buffer || count == 0 || dev->lba_size == 0) return -1;
    max_count = PAGE_SIZE / dev->lba_size;
    if (max_count == 0) return -1;

    while (count) {
        nvme_cmd_t cmd;
        uint32_t chunk = count > max_count ? max_count : count;
        uint32_t bytes = chunk * dev->lba_size;

        mem_zero((uint8_t *)&cmd, sizeof(cmd));
        if (write) {
            for (uint32_t i = 0; i < bytes; i++) dev->dma_virt[i] = ptr[i];
        }

        cmd.opcode = write ? NVME_NVM_OP_WRITE : NVME_NVM_OP_READ;
        cmd.nsid = dev->namespace_id;
        cmd.prp1 = dev->dma_phys;
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = chunk - 1U;

        if (nvme_io_submit(dev, &cmd) != 0) return -1;
        if (!write) {
            for (uint32_t i = 0; i < bytes; i++) ptr[i] = dev->dma_virt[i];
        }

        ptr += bytes;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int nvme_block_read(void *context, uint64_t lba, uint32_t count, void *buffer) {
    nvme_device_t *dev = (nvme_device_t *)context;
    if (!dev || lba + count > dev->sector_count) return -1;
    return nvme_rw(dev, lba, count, buffer, 0);
}

static int nvme_block_write(void *context, uint64_t lba, uint32_t count, const void *buffer) {
    nvme_device_t *dev = (nvme_device_t *)context;
    if (!dev || lba + count > dev->sector_count) return -1;
    return nvme_rw(dev, lba, count, (void *)buffer, 1);
}

static int nvme_setup_device(const pci_device_t *pci, uint32_t index) {
    nvme_device_t *dev = &nvme_devices[index];
    uint32_t bar0 = pci_read_config32(pci, 0x10);
    uint32_t bar1 = pci_read_config32(pci, 0x14);
    uint64_t mmio_phys = ((uint64_t)bar1 << 32) | (uint64_t)(bar0 & ~0xFUL);
    uint64_t cap;
    uint32_t cc;
    uint64_t region_phys;
    nvme_identify_ns_t ns;
    uint8_t flbas;
    uint8_t lbaf_index;

    if (!mmio_phys) {
        nvme_trace("no mmio bar");
        return -1;
    }
    mem_zero((uint8_t *)dev, sizeof(*dev));
    dev->pci = pci;
    dev->regs = (volatile uint8_t *)vmm_map_physical(mmio_phys, PAGE_SIZE_4K * 2, VMM_FLAGS_KERNEL_RW | PTE_NO_CACHE);
    cap = nvme_reg64(dev, NVME_REG_CAP);
    if (cap == 0 || cap == 0xFFFFFFFFFFFFFFFFULL) {
        /* BAR mapped but controller not responding (e.g. device asleep
         * or decode not enabled): bail instead of spinning the full
         * bounded wait. */
        nvme_trace("controller not responding (cap invalid)");
        return -1;
    }
    dev->dstrd_bytes = 4U << (uint32_t)((cap >> 32) & 0xFU);
    if (dev->dstrd_bytes == 0) dev->dstrd_bytes = 4;
    dev->admin_phase = 1;
    dev->io_phase = 1;

    (void)pci_enable_memory_busmaster(pci);

    cc = nvme_reg32(dev, NVME_REG_CC);
    cc &= ~NVME_CC_EN;
    nvme_reg32_write(dev, NVME_REG_CC, cc);
    if (nvme_wait_ready(dev, 0, 5000000U) != 0) {
        nvme_trace("disable wait failed");
        return -1;
    }

    region_phys = pmm_alloc_contiguous(5);
    if (!region_phys) {
        nvme_trace("queue alloc failed");
        return -1;
    }
    mem_zero((uint8_t *)PHYS_TO_VIRT(region_phys), 5 * PAGE_SIZE);
    dev->admin_sq_phys = region_phys;
    dev->admin_cq_phys = region_phys + PAGE_SIZE;
    dev->io_sq_phys = region_phys + (2 * PAGE_SIZE);
    dev->io_cq_phys = region_phys + (3 * PAGE_SIZE);
    dev->identify_phys = region_phys + (4 * PAGE_SIZE);

    region_phys = pmm_alloc_contiguous(1);
    if (!region_phys) {
        nvme_trace("dma alloc failed");
        return -1;
    }
    dev->dma_phys = region_phys;
    dev->dma_virt = (uint8_t *)PHYS_TO_VIRT(region_phys);
    dev->dma_bytes = PAGE_SIZE;
    dev->identify_virt = (uint8_t *)PHYS_TO_VIRT(dev->identify_phys);

    nvme_reg32_write(dev, NVME_REG_AQA, ((NVME_ADMIN_Q_DEPTH - 1U) << 16) | (NVME_ADMIN_Q_DEPTH - 1U));
    nvme_reg64_write(dev, NVME_REG_ASQ, dev->admin_sq_phys);
    nvme_reg64_write(dev, NVME_REG_ACQ, dev->admin_cq_phys);

    cc = (6U << 16) | (4U << 20) | NVME_CC_EN;
    nvme_reg32_write(dev, NVME_REG_CC, cc);
    if (nvme_wait_ready(dev, 1, 5000000U) != 0) {
        nvme_trace("enable wait failed");
        return -1;
    }

    if (nvme_setup_io_queues(dev) != 0) {
        nvme_trace("io queue setup failed");
        return -1;
    }
    if (nvme_identify_namespace(dev, 1, &ns) != 0) {
        nvme_trace("identify ns failed");
        return -1;
    }
    if (ns.ncap == 0) {
        nvme_trace("namespace empty");
        return -1;
    }

    flbas = ns.flbas;
    lbaf_index = flbas & 0x0F;
    dev->lba_size = 1U << ns.lbaf[lbaf_index].lbads;
    if (dev->lba_size == 0 || dev->lba_size > PAGE_SIZE) {
        nvme_trace("invalid lba size");
        return -1;
    }
    dev->sector_count = ns.nsze;
    dev->namespace_id = 1;
    dev->present = 1;
    dev->name[0] = 'n'; dev->name[1] = 'v'; dev->name[2] = 'm'; dev->name[3] = 'e';
    dev->name[4] = (char)('0' + index);
    dev->name[5] = 0;

    dev->block.name = dev->name;
    dev->block.context = dev;
    dev->block.sector_size = dev->lba_size;
    dev->block.sector_count = dev->sector_count;
    dev->block.read = nvme_block_read;
    dev->block.write = nvme_block_write;
    return block_register(&dev->block);
}

int nvme_init(void) {
    int any = 0;
    nvme_count = 0;
    for (uint32_t i = 0; i < pci_device_count() && nvme_count < NVME_MAX_DEVICES; i++) {
        const pci_device_t *pci = pci_device_at(i);
        if (!pci) continue;
        if (pci->class_code != NVME_CLASS_STORAGE || pci->subclass != NVME_SUBCLASS_NVM || pci->prog_if != NVME_PROGIF_NVME) {
            continue;
        }
        if (nvme_setup_device(pci, nvme_count) == 0) {
            nvme_count++;
            any = 1;
        }
    }
    return any ? 0 : -1;
}

uint32_t nvme_device_count(void) {
    return nvme_count;
}

block_device_t *nvme_block_device(uint32_t index) {
    if (index >= nvme_count || !nvme_devices[index].present) return 0;
    return &nvme_devices[index].block;
}
