#include "pci.h"

#include "../../cpu/lapic.h"
#include "../../firmware/acpi.h"
#include "../../memory/vmm.h"

#define PCI_MAX_DEVICES 128
#define PCI_CFG_CAP_PTR 0x34
#define PCI_STATUS_REG  0x06
#define PCI_STATUS_CAPS (1U << 4)
#define PCI_CAP_ID_MSI  0x05
#define PCI_CONF1_ADDR 0xCF8
#define PCI_CONF1_DATA 0xCFC

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static uint32_t pci_count = 0;
static uint32_t pci_msi_count = 0;

static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t pci_conf1_address(const pci_device_t *device, uint16_t offset) {
    return 0x80000000U |
           ((uint32_t)device->bus << 16) |
           ((uint32_t)device->device << 11) |
           ((uint32_t)device->function << 8) |
           (offset & ~0x3U);
}

uint8_t pci_read_config8(const pci_device_t *device, uint16_t offset) {
    uint32_t value = pci_read_config32(device, offset);
    return (uint8_t)((value >> ((offset & 0x3U) * 8)) & 0xFFU);
}

uint16_t pci_read_config16(const pci_device_t *device, uint16_t offset) {
    uint32_t value = pci_read_config32(device, offset);
    return (uint16_t)((value >> ((offset & 0x2U) * 8)) & 0xFFFFU);
}

uint32_t pci_read_config32(const pci_device_t *device, uint16_t offset) {
    outl(PCI_CONF1_ADDR, pci_conf1_address(device, offset));
    return inl(PCI_CONF1_DATA);
}

void pci_write_config16(const pci_device_t *device, uint16_t offset, uint16_t value) {
    uint32_t shift = (offset & 0x2U) * 8;
    uint32_t mask = 0xFFFFU << shift;
    uint32_t current = pci_read_config32(device, offset);
    uint32_t next = (current & ~mask) | (((uint32_t)value << shift) & mask);
    pci_write_config32(device, offset, next);
}

void pci_write_config32(const pci_device_t *device, uint16_t offset, uint32_t value) {
    outl(PCI_CONF1_ADDR, pci_conf1_address(device, offset));
    outl(PCI_CONF1_DATA, value);
}

int pci_enable_memory_busmaster(const pci_device_t *device) {
    uint16_t cmd;
    if (!device) return -1;
    cmd = pci_read_config16(device, 0x04);
    cmd |= (1U << 0) | (1U << 1) | (1U << 2);
    pci_write_config16(device, 0x04, cmd);
    return 0;
}

static int pci_record_device(uint16_t segment_group, uint8_t bus, uint8_t device_num,
                             uint8_t function, uint64_t cfg_phys) {
    pci_device_t *device;

    if (pci_count >= PCI_MAX_DEVICES) {
        return -1;
    }

    device = &pci_devices[pci_count++];
    device->segment_group = segment_group;
    device->bus = bus;
    device->device = device_num;
    device->function = function;
    device->cfg_phys = cfg_phys;
    device->vendor_id = pci_read_config16(device, 0x00);
    device->device_id = pci_read_config16(device, 0x02);
    device->revision_id = pci_read_config8(device, 0x08);
    device->prog_if = pci_read_config8(device, 0x09);
    device->subclass = pci_read_config8(device, 0x0A);
    device->class_code = pci_read_config8(device, 0x0B);
    device->header_type = pci_read_config8(device, 0x0E) & 0x7F;

    if (pci_device_supports_msi(device)) {
        pci_msi_count++;
    }

    return 0;
}

static uint64_t pci_cfg_phys(const struct acpi_mcfg_entry *entry, uint8_t bus,
                             uint8_t device, uint8_t function) {
    return entry->base_address +
           ((uint64_t)(bus - entry->start_bus) << 20) +
           ((uint64_t)device << 15) +
           ((uint64_t)function << 12);
}

static void pci_enumerate_bus(const struct acpi_mcfg_entry *entry, uint8_t bus) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        /* Every probe must carry the bus/device/function it intends to
         * read: pci_read_config* builds the CONFIG1 address from those
         * fields (cfg_phys is recorded for MMIO use but reads go through
         * CONFIG1).  Leaving them zero probed device 0:0:0 for every
         * slot, which both invented phantom devices with vendor 0xFFFF
         * and mis-read the multi-function flag of every device. */
        pci_device_t probe = {
            .bus = bus,
            .device = dev,
            .function = 0,
            .cfg_phys = pci_cfg_phys(entry, bus, dev, 0)
        };
        uint16_t vendor = pci_read_config16(&probe, 0x00);
        uint8_t function_limit = 1;

        if (vendor == 0xFFFF) {
            continue;
        }

        if (pci_read_config8(&probe, 0x0E) & 0x80) {
            function_limit = 8;
        }

        for (uint8_t fn = 0; fn < function_limit; fn++) {
            pci_device_t fn_probe = {
                .bus = bus,
                .device = dev,
                .function = fn,
                .cfg_phys = pci_cfg_phys(entry, bus, dev, fn)
            };

            if (pci_read_config16(&fn_probe, 0x00) == 0xFFFF) {
                continue;
            }
            if (pci_record_device(entry->segment_group, bus, dev, fn, fn_probe.cfg_phys) != 0) {
                return;
            }
        }
    }
}

static void pci_enumerate_entry(const struct acpi_mcfg_entry *entry) {
    for (uint8_t bus = entry->start_bus; bus <= entry->end_bus; bus++) {
        pci_enumerate_bus(entry, bus);
    }
}

static void pci_legacy_enumerate(void) {
    for (uint16_t bus = 0; bus < 256 && pci_count < PCI_MAX_DEVICES; bus++) {
        for (uint8_t dev = 0; dev < 32 && pci_count < PCI_MAX_DEVICES; dev++) {
            pci_device_t probe = {0};
            uint16_t vendor;
            uint8_t function_limit = 1;

            probe.bus = (uint8_t)bus;
            probe.device = dev;
            probe.function = 0;

            vendor = pci_read_config16(&probe, 0x00);
            if (vendor == 0xFFFF) {
                continue;
            }

            if (pci_read_config8(&probe, 0x0E) & 0x80U) {
                function_limit = 8;
            }

            for (uint8_t fn = 0; fn < function_limit && pci_count < PCI_MAX_DEVICES; fn++) {
                pci_device_t fn_probe = {0};
                fn_probe.bus = (uint8_t)bus;
                fn_probe.device = dev;
                fn_probe.function = fn;
                if (pci_read_config16(&fn_probe, 0x00) == 0xFFFF) {
                    continue;
                }
                if (pci_record_device(0, (uint8_t)bus, dev, fn, 0) != 0) {
                    return;
                }
            }
        }
    }
}

int pci_init(void) {
    const struct acpi_mcfg *mcfg = acpi_mcfg();
    const uint8_t *ptr;
    const uint8_t *end;

    pci_count = 0;
    pci_msi_count = 0;

    if (!mcfg) {
        pci_legacy_enumerate();
        return pci_count > 0 ? 0 : -1;
    }

    ptr = mcfg->entries;
    end = ((const uint8_t *)mcfg) + mcfg->header.length;

    while (ptr + sizeof(struct acpi_mcfg_entry) <= end) {
        const struct acpi_mcfg_entry *entry = (const struct acpi_mcfg_entry *)ptr;
        pci_enumerate_entry(entry);
        ptr += sizeof(struct acpi_mcfg_entry);
    }

    if (pci_count == 0) {
        pci_legacy_enumerate();
    }

    return pci_count > 0 ? 0 : -1;
}

uint32_t pci_device_count(void) {
    return pci_count;
}

uint32_t pci_msi_capable_count(void) {
    return pci_msi_count;
}

const pci_device_t *pci_device_at(uint32_t index) {
    if (index >= pci_count) {
        return 0;
    }
    return &pci_devices[index];
}

const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (uint32_t i = 0; i < pci_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass) {
            return &pci_devices[i];
        }
    }
    return 0;
}

const pci_device_t *pci_find_class_prog_if(uint8_t class_code, uint8_t subclass,
                                           uint8_t prog_if) {
    for (uint32_t i = 0; i < pci_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass &&
            pci_devices[i].prog_if == prog_if) {
            return &pci_devices[i];
        }
    }
    return 0;
}

int pci_bar64(const pci_device_t *device, uint8_t bar_index, uint64_t *phys_out,
              uint64_t *size_out) {
    uint16_t offset;
    uint32_t orig_lo;
    uint32_t orig_hi = 0;
    uint32_t mask_lo;
    uint32_t mask_hi = 0;
    uint16_t saved_cmd;
    uint64_t phys;
    uint64_t combined;
    uint64_t size;
    int is_64bit;

    if (!device || !phys_out || !size_out) {
        return -1;
    }
    if (bar_index >= 6) {
        return -1;
    }
    offset = (uint16_t)(0x10 + (uint16_t)bar_index * 4U);

    orig_lo = pci_read_config32(device, offset);
    if (orig_lo & 0x1U) {
        return -1; /* I/O BAR: not a 64-bit MMIO BAR */
    }
    is_64bit = (((orig_lo >> 1) & 0x3U) == 0x2U) ? 1 : 0;
    if (is_64bit) {
        if (bar_index >= 5) {
            return -1; /* upper half missing */
        }
        orig_hi = pci_read_config32(device, (uint16_t)(offset + 4));
    }

    /* Disable MEM+IO decode during the sizing probe so the all-ones
     * write cannot claim a live address decode. Restored below. */
    saved_cmd = pci_read_config16(device, 0x04);
    pci_write_config16(device, 0x04, (uint16_t)(saved_cmd & ~0x3U));

    pci_write_config32(device, offset, 0xFFFFFFFFU);
    mask_lo = pci_read_config32(device, offset);
    if (is_64bit) {
        pci_write_config32(device, (uint16_t)(offset + 4), 0xFFFFFFFFU);
        mask_hi = pci_read_config32(device, (uint16_t)(offset + 4));
    }

    /* Restore original BAR values before any early return. */
    pci_write_config32(device, offset, orig_lo);
    if (is_64bit) {
        pci_write_config32(device, (uint16_t)(offset + 4), orig_hi);
    }
    pci_write_config16(device, 0x04, saved_cmd);

    mask_lo &= ~0xFU;
    if (is_64bit) {
        combined = ((uint64_t)mask_hi << 32) | (uint64_t)mask_lo;
    } else {
        combined = (uint64_t)mask_lo;
    }
    if (combined == 0) {
        return -1; /* unimplemented BAR */
    }

    phys = is_64bit ? (((uint64_t)orig_hi << 32) | (uint64_t)(orig_lo & ~0xFU))
                    : (uint64_t)(orig_lo & ~0xFU);
    if (phys == 0) {
        return -1; /* BAR unprogrammed */
    }

    size = ~combined + 1ULL;
    if (size == 0) {
        return -1;
    }

    *phys_out = phys;
    *size_out = size;
    return 0;
}

int pci_enable_busmaster_mmio(const pci_device_t *device) {
    uint16_t cmd;
    if (!device) {
        return -1;
    }
    cmd = pci_read_config16(device, 0x04);
    cmd |= (1U << 0) | (1U << 1) | (1U << 2); /* IO + MEM + bus master */
    cmd |= (1U << 10); /* disable legacy INTx for the MSI path */
    pci_write_config16(device, 0x04, cmd);
    return 0;
}

int pci_find_capability(const pci_device_t *device, uint8_t cap_id, uint8_t *offset_out) {
    uint16_t status;
    uint8_t cap_ptr;
    int guard = 0;

    if (!device) {
        return -1;
    }

    status = pci_read_config16(device, PCI_STATUS_REG);
    if (!(status & PCI_STATUS_CAPS)) {
        return -1;
    }

    cap_ptr = pci_read_config8(device, PCI_CFG_CAP_PTR) & ~0x3U;
    while (cap_ptr >= 0x40 && cap_ptr != 0 && guard++ < 48) {
        uint8_t current_id = pci_read_config8(device, cap_ptr);
        uint8_t next = pci_read_config8(device, cap_ptr + 1) & ~0x3U;
        if (current_id == cap_id) {
            if (offset_out) {
                *offset_out = cap_ptr;
            }
            return 0;
        }
        cap_ptr = next;
    }

    return -1;
}

int pci_device_supports_msi(const pci_device_t *device) {
    return pci_find_capability(device, PCI_CAP_ID_MSI, 0) == 0;
}

int pci_enable_msi(const pci_device_t *device, uint8_t vector) {
    uint8_t cap;
    uint16_t control;
    uint32_t msg_addr_lo;
    uint32_t msg_addr_hi = 0;
    uint16_t msg_data;
    uint32_t dest_id;
    int is_64bit;

    if (!device || pci_find_capability(device, PCI_CAP_ID_MSI, &cap) != 0) {
        return -1;
    }

    control = pci_read_config16(device, cap + 2);
    is_64bit = (control & (1U << 7)) != 0;
    dest_id = lapic_id() & 0xFFU;

    msg_addr_lo = 0xFEE00000U | (dest_id << 12);
    msg_data = vector;

    pci_write_config32(device, cap + 4, msg_addr_lo);
    if (is_64bit) {
        pci_write_config32(device, cap + 8, msg_addr_hi);
        pci_write_config16(device, cap + 12, msg_data);
    } else {
        pci_write_config16(device, cap + 8, msg_data);
    }

    control &= ~(0x7U << 4);
    control |= 1U;
    pci_write_config16(device, cap + 2, control);
    return 0;
}

int pci_enable_msi_vector(const pci_device_t *device, uint8_t vector) {
    if (!device) {
        return -1;
    }
    return pci_enable_msi(device, vector);
}
