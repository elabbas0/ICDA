#include "ioapic.h"

#include "../memory/vmm.h"

#define IOAPIC_REGSEL 0x00
#define IOAPIC_WINDOW 0x10
#define IOAPIC_REG_VER 0x01
#define IOAPIC_REDIR_BASE 0x10

#define MADT_TYPE_IOAPIC        1
#define MADT_TYPE_INTERRUPT_SRC 2

typedef struct {
    uint8_t id;
    uint32_t gsi_base;
    uint32_t gsi_max;
    volatile uint32_t *regs;
} ioapic_desc_t;

typedef struct {
    uint8_t present;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
} irq_override_t;

static ioapic_desc_t ioapics[4];
static int ioapic_count = 0;
static irq_override_t overrides[16];

static uint32_t ioapic_read(ioapic_desc_t *ioapic, uint8_t reg) {
    ioapic->regs[IOAPIC_REGSEL / 4] = reg;
    return ioapic->regs[IOAPIC_WINDOW / 4];
}

static void ioapic_write(ioapic_desc_t *ioapic, uint8_t reg, uint32_t value) {
    ioapic->regs[IOAPIC_REGSEL / 4] = reg;
    ioapic->regs[IOAPIC_WINDOW / 4] = value;
}

static ioapic_desc_t *find_ioapic_for_gsi(uint32_t gsi) {
    for (int i = 0; i < ioapic_count; i++) {
        if (gsi >= ioapics[i].gsi_base && gsi <= ioapics[i].gsi_max) {
            return &ioapics[i];
        }
    }
    return 0;
}

static uint32_t resolve_gsi(uint8_t irq, uint16_t *flags) {
    if (irq < 16 && overrides[irq].present) {
        if (flags) {
            *flags = overrides[irq].flags;
        }
        return overrides[irq].gsi;
    }

    if (flags) {
        *flags = 0;
    }
    return irq;
}

int ioapic_init(const struct acpi_madt *madt) {
    const uint8_t *ptr = madt->entries;
    const uint8_t *end = ((const uint8_t *)madt) + madt->header.length;

    for (int i = 0; i < 16; i++) {
        overrides[i].present = 0;
        overrides[i].source_irq = 0;
        overrides[i].gsi = 0;
        overrides[i].flags = 0;
    }
    ioapic_count = 0;

    while (ptr + sizeof(struct acpi_madt_entry_header) <= end) {
        const struct acpi_madt_entry_header *hdr = (const struct acpi_madt_entry_header *)ptr;
        if (hdr->length < sizeof(struct acpi_madt_entry_header) || ptr + hdr->length > end) {
            break;
        }

        if (hdr->type == MADT_TYPE_IOAPIC && ioapic_count < 4) {
            const struct acpi_madt_ioapic *entry = (const struct acpi_madt_ioapic *)ptr;
            ioapic_desc_t *ioapic = &ioapics[ioapic_count++];
            ioapic->id = entry->ioapic_id;
            ioapic->gsi_base = entry->gsi_base;
            ioapic->regs = (volatile uint32_t *)vmm_map_physical(
                entry->ioapic_address, PAGE_SIZE_4K, VMM_WRITE | PTE_NO_CACHE | PTE_WRITE_THRU);
            if (!ioapic->regs) {
                return -1;
            }
            uint32_t version = ioapic_read(ioapic, IOAPIC_REG_VER);
            ioapic->gsi_max = ioapic->gsi_base + ((version >> 16) & 0xFF);
        } else if (hdr->type == MADT_TYPE_INTERRUPT_SRC) {
            const struct acpi_madt_iso *entry = (const struct acpi_madt_iso *)ptr;
            if (entry->source_irq < 16) {
                overrides[entry->source_irq].present = 1;
                overrides[entry->source_irq].source_irq = entry->source_irq;
                overrides[entry->source_irq].gsi = entry->gsi;
                overrides[entry->source_irq].flags = entry->flags;
            }
        }

        ptr += hdr->length;
    }

    return ioapic_count > 0 ? 0 : -1;
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id, int masked) {
    uint16_t flags = 0;
    uint32_t gsi = resolve_gsi(irq, &flags);
    ioapic_desc_t *ioapic = find_ioapic_for_gsi(gsi);
    uint32_t index;
    uint32_t low;
    uint32_t high;

    if (!ioapic) {
        return;
    }

    index = gsi - ioapic->gsi_base;
    low = vector;
    high = ((uint32_t)lapic_id) << 24;

    if ((flags & 0x3) == 0x3) {
        low |= 1U << 13;
    }
    if (((flags >> 2) & 0x3) == 0x3) {
        low |= 1U << 15;
    }
    if (masked) {
        low |= 1U << 16;
    }

    ioapic_write(ioapic, (uint8_t)(IOAPIC_REDIR_BASE + index * 2 + 1), high);
    ioapic_write(ioapic, (uint8_t)(IOAPIC_REDIR_BASE + index * 2), low);
}

void ioapic_mask_irq(uint8_t irq) {
    ioapic_route_irq(irq, 32 + irq, 0, 1);
}

void ioapic_unmask_irq(uint8_t irq, uint8_t vector, uint8_t lapic_id) {
    ioapic_route_irq(irq, vector, lapic_id, 0);
}
