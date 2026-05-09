#include "acpi.h"

#include "../cpu/multiboot2.h"
#include "../memory/vmm.h"

static const struct acpi_rsdp *g_rsdp = 0;
static const struct acpi_madt *g_madt = 0;
static const struct acpi_mcfg *g_mcfg = 0;

static int checksum_ok(const void *ptr, uint32_t length) {
    const uint8_t *bytes = (const uint8_t *)ptr;
    uint8_t sum = 0;

    for (uint32_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }

    return sum == 0;
}

static int signature_eq(const char *a, const char *b, int count) {
    for (int i = 0; i < count; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static const struct acpi_rsdp *map_rsdp(uint64_t phys) {
    const struct acpi_rsdp *rsdp =
        (const struct acpi_rsdp *)vmm_map_physical(phys, sizeof(struct acpi_rsdp),
                                                   VMM_WRITE | PTE_NO_CACHE);
    if (!rsdp) {
        return 0;
    }

    if (!signature_eq(rsdp->signature, "RSD PTR ", 8)) {
        return 0;
    }
    if (!checksum_ok(rsdp, 20)) {
        return 0;
    }
    if (rsdp->revision >= 2 && (!rsdp->length || !checksum_ok(rsdp, rsdp->length))) {
        return 0;
    }

    return rsdp;
}

static const struct acpi_rsdp *find_rsdp_in_multiboot(void *multiboot_info) {
    struct multiboot_info *info = (struct multiboot_info *)multiboot_info;
    uint8_t *tag_ptr = (uint8_t *)multiboot_info + 8;
    uint8_t *end_ptr = (uint8_t *)multiboot_info + info->total_size;

    for (uint8_t *p = tag_ptr; p < end_ptr; ) {
        struct multiboot_tag *tag = (struct multiboot_tag *)p;
        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            break;
        }

        if (tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW ||
            tag->type == MULTIBOOT_TAG_TYPE_ACPI_OLD) {
            struct multiboot_tag_acpi *acpi_tag = (struct multiboot_tag_acpi *)tag;
            const struct acpi_rsdp *rsdp = (const struct acpi_rsdp *)acpi_tag->rsdp;
            if (signature_eq(rsdp->signature, "RSD PTR ", 8) &&
                checksum_ok(rsdp, (tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW && rsdp->length)
                                      ? rsdp->length
                                      : 20)) {
                return rsdp;
            }
        }

        p += (tag->size + 7) & ~7U;
    }

    return 0;
}

static const struct acpi_rsdp *scan_rsdp_range(uint64_t start, uint64_t end) {
    for (uint64_t phys = start; phys + 16 <= end; phys += 16) {
        const struct acpi_rsdp *rsdp = map_rsdp(phys);
        if (rsdp) {
            return rsdp;
        }
    }
    return 0;
}

static const struct acpi_rsdp *find_rsdp_legacy(void) {
    uint16_t *ebda_segment = (uint16_t *)(uintptr_t)0x40E;
    uint64_t ebda_phys = ((uint64_t)(*ebda_segment)) << 4;
    const struct acpi_rsdp *rsdp = 0;

    if (ebda_phys >= 0x80000 && ebda_phys < 0xA0000) {
        rsdp = scan_rsdp_range(ebda_phys, ebda_phys + 1024);
        if (rsdp) {
            return rsdp;
        }
    }

    return scan_rsdp_range(0xE0000, 0x100000);
}

const struct acpi_sdt_header *acpi_find_table(const char signature[4]) {
    const struct acpi_sdt_header *root;
    uint64_t entry_phys;
    uint32_t entry_size;
    uint32_t entry_count;

    if (!g_rsdp) {
        return 0;
    }

    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_address) {
        root = (const struct acpi_sdt_header *)
            vmm_map_physical(g_rsdp->xsdt_address, sizeof(struct acpi_sdt_header),
                             VMM_WRITE | PTE_NO_CACHE);
        entry_size = 8;
    } else {
        root = (const struct acpi_sdt_header *)
            vmm_map_physical(g_rsdp->rsdt_address, sizeof(struct acpi_sdt_header),
                             VMM_WRITE | PTE_NO_CACHE);
        entry_size = 4;
    }

    if (!root) {
        return 0;
    }

    root = (const struct acpi_sdt_header *)
        vmm_map_physical((g_rsdp->revision >= 2 && g_rsdp->xsdt_address)
                             ? g_rsdp->xsdt_address
                             : g_rsdp->rsdt_address,
                         root->length, VMM_WRITE | PTE_NO_CACHE);
    if (!root || !checksum_ok(root, root->length)) {
        return 0;
    }

    entry_count = (root->length - sizeof(struct acpi_sdt_header)) / entry_size;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (entry_size == 8) {
            const uint64_t *entries = (const uint64_t *)((const uint8_t *)root + sizeof(struct acpi_sdt_header));
            entry_phys = entries[i];
        } else {
            const uint32_t *entries = (const uint32_t *)((const uint8_t *)root + sizeof(struct acpi_sdt_header));
            entry_phys = entries[i];
        }

        const struct acpi_sdt_header *table =
            (const struct acpi_sdt_header *)vmm_map_physical(entry_phys, sizeof(struct acpi_sdt_header),
                                                             VMM_WRITE | PTE_NO_CACHE);
        if (!table) {
            continue;
        }

        table = (const struct acpi_sdt_header *)vmm_map_physical(entry_phys, table->length,
                                                                 VMM_WRITE | PTE_NO_CACHE);
        if (!table || !checksum_ok(table, table->length)) {
            continue;
        }

        if (signature_eq(table->signature, signature, 4)) {
            return table;
        }
    }

    return 0;
}

int acpi_init(void *multiboot_info) {
    g_rsdp = find_rsdp_in_multiboot(multiboot_info);
    if (!g_rsdp) {
        g_rsdp = find_rsdp_legacy();
    }
    if (!g_rsdp) {
        return -1;
    }

    g_madt = (const struct acpi_madt *)acpi_find_table("APIC");
    if (!g_madt) {
        return -1;
    }
    g_mcfg = (const struct acpi_mcfg *)acpi_find_table("MCFG");

    return 0;
}

const struct acpi_rsdp *acpi_rsdp(void) {
    return g_rsdp;
}

const struct acpi_madt *acpi_madt(void) {
    return g_madt;
}

const struct acpi_mcfg *acpi_mcfg(void) {
    return g_mcfg;
}
