#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t lapic_address;
    uint32_t flags;
    uint8_t entries[0];
} __attribute__((packed));

struct acpi_madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct acpi_madt_lapic {
    uint8_t type;
    uint8_t length;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

struct acpi_madt_ioapic {
    uint8_t type;
    uint8_t length;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
} __attribute__((packed));

struct acpi_madt_iso {
    uint8_t type;
    uint8_t length;
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

struct acpi_madt_lapic_override {
    uint8_t type;
    uint8_t length;
    uint16_t reserved;
    uint64_t lapic_address;
} __attribute__((packed));

struct acpi_mcfg {
    struct acpi_sdt_header header;
    uint64_t reserved;
    uint8_t entries[0];
} __attribute__((packed));

struct acpi_mcfg_entry {
    uint64_t base_address;
    uint16_t segment_group;
    uint8_t start_bus;
    uint8_t end_bus;
    uint32_t reserved;
} __attribute__((packed));

int acpi_init(void *multiboot_info);
const struct acpi_rsdp *acpi_rsdp(void);
const struct acpi_sdt_header *acpi_find_table(const char signature[4]);
const struct acpi_madt *acpi_madt(void);
const struct acpi_mcfg *acpi_mcfg(void);

#endif
