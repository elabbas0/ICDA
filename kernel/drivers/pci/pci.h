#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint16_t segment_group;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision_id;
    uint8_t header_type;
    uint64_t cfg_phys;
} pci_device_t;

int pci_init(void);
uint32_t pci_device_count(void);
uint32_t pci_msi_capable_count(void);
const pci_device_t *pci_device_at(uint32_t index);
const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);
uint8_t pci_read_config8(const pci_device_t *device, uint16_t offset);
uint16_t pci_read_config16(const pci_device_t *device, uint16_t offset);
uint32_t pci_read_config32(const pci_device_t *device, uint16_t offset);
void pci_write_config16(const pci_device_t *device, uint16_t offset, uint16_t value);
void pci_write_config32(const pci_device_t *device, uint16_t offset, uint32_t value);
int pci_enable_memory_busmaster(const pci_device_t *device);
int pci_find_capability(const pci_device_t *device, uint8_t cap_id, uint8_t *offset_out);
int pci_device_supports_msi(const pci_device_t *device);
int pci_enable_msi(const pci_device_t *device, uint8_t vector);

#endif
