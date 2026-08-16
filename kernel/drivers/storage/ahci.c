#include "ahci.h"

#include "../pci/pci.h"
#include "../../memory/pmm.h"
#include "../../memory/vmm.h"

#include <stdint.h>

#define AHCI_CLASS_STORAGE 0x01
#define AHCI_SUBCLASS_SATA 0x06
#define AHCI_PROGIF_AHCI   0x01

#define AHCI_MAX_DEVICES   8
#define AHCI_SECTOR_SIZE   512U
#define AHCI_DMA_PAGES     4U
#define AHCI_DMA_SECTORS   ((AHCI_DMA_PAGES * PAGE_SIZE) / AHCI_SECTOR_SIZE)

#define SATA_SIG_ATA       0x00000101U

#define HBA_GHC_AE         (1U << 31)
#define HBA_PXCMD_ST       (1U << 0)
#define HBA_PXCMD_FRE      (1U << 4)
#define HBA_PXCMD_FR       (1U << 14)
#define HBA_PXCMD_CR       (1U << 15)
#define HBA_PXIS_TFES      (1U << 30)

#define ATA_CMD_IDENTIFY_DEVICE 0xEC
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35

#define ATA_DEV_BUSY       0x80
#define ATA_DEV_DRQ        0x08

typedef volatile struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t devslp;
    uint32_t rsv1[10];
    uint32_t vendor[4];
} hba_port_t;

typedef volatile struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t rsv[0xA0 - 0x2C];
    uint8_t vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct {
    uint8_t cfl;
    uint8_t flags;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    uint8_t fis_type;
    uint8_t pmport:4;
    uint8_t rsv0:3;
    uint8_t c:1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc_i;
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt[1];
} __attribute__((packed)) hba_cmd_tbl_t;

typedef struct {
    int present;
    uint8_t port_no;
    hba_port_t *port;
    uint64_t cmdlist_phys;
    uint64_t rfis_phys;
    uint64_t cmdtbl_phys;
    uint64_t dma_phys;
    uint8_t *dma_virt;
    uint64_t sector_count;
    block_device_t block;
} ahci_device_t;

static ahci_device_t ahci_devices[AHCI_MAX_DEVICES];
static uint32_t ahci_count = 0;

static void mem_zero(uint8_t *dst, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) dst[i] = 0;
}

static int wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t limit) {
    while (limit--) {
        if (((*reg) & mask) == 0) return 0;
    }
    return -1;
}

static int ahci_port_stop(hba_port_t *port) {
    port->cmd &= ~HBA_PXCMD_ST;
    if (wait_clear(&port->cmd, HBA_PXCMD_CR, 1000000) != 0) return -1;
    port->cmd &= ~HBA_PXCMD_FRE;
    if (wait_clear(&port->cmd, HBA_PXCMD_FR, 1000000) != 0) return -1;
    return 0;
}

static void ahci_port_start(hba_port_t *port) {
    port->cmd |= HBA_PXCMD_FRE;
    port->cmd |= HBA_PXCMD_ST;
}

static int ahci_port_ready(hba_port_t *port) {
    uint32_t ssts = port->ssts;
    uint8_t det = (uint8_t)(ssts & 0x0F);
    uint8_t ipm = (uint8_t)((ssts >> 8) & 0x0F);
    return det == 3 && ipm == 1;
}

static int ahci_wait_idle(hba_port_t *port) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint32_t tfd = port->tfd;
        if ((tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) == 0) return 0;
    }
    return -1;
}

static int ahci_issue(ahci_device_t *dev, uint8_t command, uint64_t lba, uint16_t count, int write) {
    hba_cmd_header_t *hdr;
    hba_cmd_tbl_t *tbl;
    fis_reg_h2d_t *fis;
    uint32_t byte_count = (uint32_t)count * AHCI_SECTOR_SIZE;

    if (!dev || !dev->present || count == 0 || count > AHCI_DMA_SECTORS) return -1;
    if (ahci_wait_idle(dev->port) != 0) return -1;

    hdr = (hba_cmd_header_t *)PHYS_TO_VIRT(dev->cmdlist_phys);
    tbl = (hba_cmd_tbl_t *)PHYS_TO_VIRT(dev->cmdtbl_phys);
    mem_zero((uint8_t *)&hdr[0], sizeof(hba_cmd_header_t));
    mem_zero((uint8_t *)tbl, sizeof(hba_cmd_tbl_t));

    hdr[0].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    hdr[0].flags = write ? (1U << 6) : 0;
    hdr[0].prdtl = 1;
    hdr[0].ctba = (uint32_t)dev->cmdtbl_phys;
    hdr[0].ctbau = (uint32_t)(dev->cmdtbl_phys >> 32);

    tbl->prdt[0].dba = (uint32_t)dev->dma_phys;
    tbl->prdt[0].dbau = (uint32_t)(dev->dma_phys >> 32);
    tbl->prdt[0].dbc_i = (byte_count - 1U);

    fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type = 0x27;
    fis->c = 1;
    fis->command = command;
    fis->device = 1U << 6;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    dev->port->is = 0xFFFFFFFFU;
    dev->port->ci = 1U;

    for (uint32_t i = 0; i < 5000000; i++) {
        if ((dev->port->ci & 1U) == 0) break;
    }
    if (dev->port->ci & 1U) return -1;
    if (dev->port->is & HBA_PXIS_TFES) return -1;
    if (dev->port->tfd & 0x01U) return -1;
    return 0;
}

static int ahci_identify(ahci_device_t *dev) {
    uint16_t *id;
    uint64_t sectors = 0;

    mem_zero(dev->dma_virt, AHCI_DMA_SECTORS * AHCI_SECTOR_SIZE);
    if (ahci_issue(dev, ATA_CMD_IDENTIFY_DEVICE, 0, 1, 0) != 0) return -1;

    id = (uint16_t *)dev->dma_virt;
    sectors =
        ((uint64_t)id[103] << 48) |
        ((uint64_t)id[102] << 32) |
        ((uint64_t)id[101] << 16) |
        (uint64_t)id[100];
    if (sectors == 0) {
        sectors = ((uint64_t)id[61] << 16) | (uint64_t)id[60];
    }
    if (sectors == 0) return -1;
    dev->sector_count = sectors;
    return 0;
}

static int ahci_block_read(void *context, uint64_t lba, uint32_t count, void *buffer) {
    ahci_device_t *dev = (ahci_device_t *)context;
    uint8_t *out = (uint8_t *)buffer;

    if (!dev || !buffer || count == 0) return -1;
    if (lba + count > dev->sector_count) return -1;

    while (count) {
        uint16_t chunk = (count > AHCI_DMA_SECTORS) ? AHCI_DMA_SECTORS : (uint16_t)count;
        uint32_t bytes = (uint32_t)chunk * AHCI_SECTOR_SIZE;
        if (ahci_issue(dev, ATA_CMD_READ_DMA_EXT, lba, chunk, 0) != 0) return -1;
        for (uint32_t i = 0; i < bytes; i++) out[i] = dev->dma_virt[i];
        out += bytes;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int ahci_block_write(void *context, uint64_t lba, uint32_t count, const void *buffer) {
    ahci_device_t *dev = (ahci_device_t *)context;
    const uint8_t *in = (const uint8_t *)buffer;

    if (!dev || !buffer || count == 0) return -1;
    if (lba + count > dev->sector_count) return -1;

    while (count) {
        uint16_t chunk = (count > AHCI_DMA_SECTORS) ? AHCI_DMA_SECTORS : (uint16_t)count;
        uint32_t bytes = (uint32_t)chunk * AHCI_SECTOR_SIZE;
        for (uint32_t i = 0; i < bytes; i++) dev->dma_virt[i] = in[i];
        if (ahci_issue(dev, ATA_CMD_WRITE_DMA_EXT, lba, chunk, 1) != 0) return -1;
        in += bytes;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int ahci_setup_device(const pci_device_t *pci_dev, hba_mem_t *abar, uint8_t port_no) {
    ahci_device_t *dev;
    uint64_t region_phys;
    uint8_t *region;
    hba_port_t *port = &abar->ports[port_no];
    char *name;

    if (ahci_count >= AHCI_MAX_DEVICES) return -1;
    if (!ahci_port_ready(port)) return -1;
    if (port->sig != SATA_SIG_ATA) return -1;

    dev = &ahci_devices[ahci_count];
    region_phys = pmm_alloc_contiguous(2 + AHCI_DMA_PAGES);
    if (!region_phys) return -1;
    region = (uint8_t *)PHYS_TO_VIRT(region_phys);
    mem_zero(region, (2 + AHCI_DMA_PAGES) * PAGE_SIZE);

    dev->present = 1;
    dev->port_no = port_no;
    dev->port = port;
    dev->cmdlist_phys = region_phys;
    dev->rfis_phys = region_phys + PAGE_SIZE;
    dev->cmdtbl_phys = region_phys + (2 * PAGE_SIZE);
    dev->dma_phys = region_phys + (3 * PAGE_SIZE);
    dev->dma_virt = (uint8_t *)PHYS_TO_VIRT(dev->dma_phys);

    if (ahci_port_stop(port) != 0) return -1;
    port->clb = (uint32_t)dev->cmdlist_phys;
    port->clbu = (uint32_t)(dev->cmdlist_phys >> 32);
    port->fb = (uint32_t)dev->rfis_phys;
    port->fbu = (uint32_t)(dev->rfis_phys >> 32);
    port->serr = 0xFFFFFFFFU;
    port->is = 0xFFFFFFFFU;
    ahci_port_start(port);

    if (ahci_identify(dev) != 0) {
        dev->present = 0;
        return -1;
    }

    name = (char *)PHYS_TO_VIRT(pmm_alloc());
    if (!name) return -1;
    name[0] = 'a'; name[1] = 'h'; name[2] = 'c'; name[3] = 'i';
    name[4] = (char)('0' + ahci_count);
    name[5] = 0;

    dev->block.name = name;
    dev->block.context = dev;
    dev->block.sector_size = AHCI_SECTOR_SIZE;
    dev->block.sector_count = dev->sector_count;
    dev->block.read = ahci_block_read;
    dev->block.write = ahci_block_write;
    (void)block_register(&dev->block);
    ahci_count++;
    (void)pci_dev;
    return 0;
}

int ahci_init(void) {
    int any = 0;

    for (uint32_t i = 0; i < pci_device_count(); i++) {
        const pci_device_t *dev = pci_device_at(i);
        uint32_t bar5_lo;
        uint32_t bar5_hi = 0;
        uint64_t abar_phys;
        hba_mem_t *abar;
        uint32_t pi;

        if (!dev) continue;
        if (dev->class_code != AHCI_CLASS_STORAGE || dev->subclass != AHCI_SUBCLASS_SATA || dev->prog_if != AHCI_PROGIF_AHCI) {
            continue;
        }

        (void)pci_enable_memory_busmaster(dev);
        bar5_lo = pci_read_config32(dev, 0x24);
        if ((bar5_lo & 0x6U) == 0x4U) {
            bar5_hi = pci_read_config32(dev, 0x28);
        }
        abar_phys = ((uint64_t)bar5_hi << 32) | (uint64_t)(bar5_lo & ~0xFUL);
        if (!abar_phys) continue;

        abar = (hba_mem_t *)vmm_map_physical(abar_phys, PAGE_SIZE_4K * 2, VMM_FLAGS_KERNEL_RW | PTE_NO_CACHE);
        if (!abar) continue;
        /* Controller not responding (BAR reads all-ones): skip instead of
         * poking 32 phantom ports through the bounded waits. */
        if (abar->pi == 0xFFFFFFFFU || abar->cap == 0) continue;
        abar->ghc |= HBA_GHC_AE;
        pi = abar->pi;
        for (uint8_t port = 0; port < 32; port++) {
            if ((pi & (1U << port)) == 0) continue;
            if (ahci_setup_device(dev, abar, port) == 0) any = 1;
        }
    }

    return any ? 0 : -1;
}

uint32_t ahci_device_count(void) {
    return ahci_count;
}

block_device_t *ahci_block_device(uint32_t index) {
    if (index >= ahci_count || !ahci_devices[index].present) return 0;
    return &ahci_devices[index].block;
}
