#include "acpi.h"

#include "../cpu/multiboot2.h"
#include "../memory/vmm.h"
#include "../drivers/serial/serial.h"
#include "../fs/bootlog.h"

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

/* GRUB passes the RSDP twice: an ACPI 1.0 copy (20 bytes, tag type 14) and
 * the full ACPI 2.0 one (tag type 15).  The 1.0 copy still carries the
 * firmware's revision 2, but its XSDT field lies past the 20 bytes (in the
 * next tag), so it is only ever used as revision 0, through the RSDT. */
static struct acpi_rsdp rsdp_v1;

static const struct acpi_rsdp *find_rsdp_in_multiboot(void *multiboot_info) {
    struct multiboot_info *info = (struct multiboot_info *)multiboot_info;
    uint8_t *tag_ptr = (uint8_t *)multiboot_info + 8;
    uint8_t *end_ptr = (uint8_t *)multiboot_info + info->total_size;
    const struct acpi_rsdp *v1 = 0;

    for (uint8_t *p = tag_ptr; p + 8 <= end_ptr; ) {
        struct multiboot_tag *tag = (struct multiboot_tag *)p;
        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            break;
        }
        if (tag->size < 8 || p + tag->size > end_ptr) {
            serial_write("[acpi] boot information damaged\n");
            break;
        }

        if ((tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW ||
             tag->type == MULTIBOOT_TAG_TYPE_ACPI_OLD) && tag->size >= 8 + 20) {
            struct multiboot_tag_acpi *acpi_tag = (struct multiboot_tag_acpi *)tag;
            const struct acpi_rsdp *rsdp = (const struct acpi_rsdp *)acpi_tag->rsdp;
            if (signature_eq(rsdp->signature, "RSD PTR ", 8) && checksum_ok(rsdp, 20)) {
                if (tag->type == MULTIBOOT_TAG_TYPE_ACPI_NEW && rsdp->revision >= 2 &&
                    rsdp->length >= sizeof(struct acpi_rsdp) && rsdp->length <= tag->size - 8 &&
                    checksum_ok(rsdp, rsdp->length)) {
                    return rsdp;
                }
                if (tag->type == MULTIBOOT_TAG_TYPE_ACPI_OLD && !v1) {
                    const uint8_t *s = (const uint8_t *)rsdp;
                    uint8_t *d = (uint8_t *)&rsdp_v1;
                    for (uint32_t i = 0; i < sizeof(rsdp_v1); i++) d[i] = i < 20 ? s[i] : 0;
                    rsdp_v1.revision = 0;
                    v1 = &rsdp_v1;
                }
            }
        }

        p += (tag->size + 7) & ~7U;
    }

    return v1;
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

/* Real firmware tables are trusted only as far as they check out: lengths
 * are bounded before anything is summed or walked. */
#define ACPI_MAX_TABLE_LEN (4U * 1024U * 1024U)
#define ACPI_MAX_ENTRIES   256U

static int acpi_log_tables;

static void log_hex(uint64_t v) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xF];
    buf[18] = 0;
    serial_write(buf);
}

static const struct acpi_sdt_header *map_table(uint64_t phys) {
    const struct acpi_sdt_header *t;
    if (!phys) return 0;
    t = (const struct acpi_sdt_header *)vmm_map_physical(phys, sizeof(struct acpi_sdt_header),
                                                         VMM_WRITE | PTE_NO_CACHE);
    if (!t || t->length < sizeof(struct acpi_sdt_header) || t->length > ACPI_MAX_TABLE_LEN) return 0;
    t = (const struct acpi_sdt_header *)vmm_map_physical(phys, t->length, VMM_WRITE | PTE_NO_CACHE);
    if (!t || !checksum_ok(t, t->length)) return 0;
    return t;
}

const struct acpi_sdt_header *acpi_find_table(const char signature[4]) {
    const struct acpi_sdt_header *root;
    int xsdt;
    uint32_t entry_size, entry_count;

    if (!g_rsdp) {
        return 0;
    }
    xsdt = g_rsdp->revision >= 2 && g_rsdp->xsdt_address;
    root = map_table(xsdt ? g_rsdp->xsdt_address : g_rsdp->rsdt_address);
    if (!root) {
        if (acpi_log_tables) serial_write("[acpi] root table unreadable\n");
        return 0;
    }
    entry_size = xsdt ? 8 : 4;
    entry_count = (root->length - sizeof(struct acpi_sdt_header)) / entry_size;
    if (entry_count > ACPI_MAX_ENTRIES) entry_count = ACPI_MAX_ENTRIES;

    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t *e = (const uint8_t *)root + sizeof(struct acpi_sdt_header) + i * entry_size;
        uint64_t entry_phys = 0;
        const struct acpi_sdt_header *table;
        for (uint32_t b = 0; b < entry_size; b++) entry_phys |= (uint64_t)e[b] << (8 * b);

        if (acpi_log_tables) {
            serial_write("[acpi] table at ");
            log_hex(entry_phys);
            bootlog_flush(0);
        }
        table = map_table(entry_phys);
        if (acpi_log_tables) {
            char sig[5] = { '?', '?', '?', '?', 0 };
            if (table) for (int k = 0; k < 4; k++) sig[k] = table->signature[k];
            serial_write(table ? ": " : ": invalid ");
            serial_write(sig);
            serial_write("\n");
        }
        if (table && signature_eq(table->signature, signature, 4)) {
            return table;
        }
    }

    return 0;
}

int acpi_init(void *multiboot_info) {
    g_rsdp = find_rsdp_in_multiboot(multiboot_info);
    if (!g_rsdp) {
        serial_write("[acpi] no RSDP from the boot loader, scanning low memory\n");
        g_rsdp = find_rsdp_legacy();
    }
    if (!g_rsdp) {
        serial_write("[acpi] no RSDP\n");
        return -1;
    }
    serial_write("[acpi] rsdp revision ");
    log_hex(g_rsdp->revision);
    serial_write(g_rsdp->revision >= 2 ? " xsdt " : " rsdt ");
    log_hex(g_rsdp->revision >= 2 ? g_rsdp->xsdt_address : g_rsdp->rsdt_address);
    serial_write("\n");
    bootlog_flush(0);

    acpi_log_tables = 1;
    g_madt = (const struct acpi_madt *)acpi_find_table("APIC");
    acpi_log_tables = 0;
    if (!g_madt) {
        serial_write("[acpi] no MADT\n");
        return -1;
    }
    g_mcfg = (const struct acpi_mcfg *)acpi_find_table("MCFG");
    bootlog_flush("ACPI tables read");

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
