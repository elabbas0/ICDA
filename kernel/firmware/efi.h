#ifndef EFI_H
#define EFI_H

#include <stdint.h>
#include "../drivers/storage/partition.h"

#define EFI_STATUS_NOT_FOUND 0x800000000000000EULL

void efi_init(void *multiboot_info);
int  efi_available(void);
int  efi_register_boot_entry(const partition_info_t *esp, const char *loader_path, const char *label);

#endif
