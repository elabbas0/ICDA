#ifndef BOOT_ASSETS_H
#define BOOT_ASSETS_H

#include <stdint.h>

extern const char boot_asset_efi_start[] __attribute__((weak));
extern const char boot_asset_efi_end[] __attribute__((weak));
extern const char boot_asset_grub_cfg_start[] __attribute__((weak));
extern const char boot_asset_grub_cfg_end[] __attribute__((weak));
extern const char boot_asset_kernel_bin_start[] __attribute__((weak));
extern const char boot_asset_kernel_bin_end[] __attribute__((weak));

static inline uint64_t boot_asset_efi_size(void) {
    return (uint64_t)(boot_asset_efi_end - boot_asset_efi_start);
}

static inline uint64_t boot_asset_grub_cfg_size(void) {
    return (uint64_t)(boot_asset_grub_cfg_end - boot_asset_grub_cfg_start);
}

static inline uint64_t boot_asset_kernel_bin_size(void) {
    return (uint64_t)(boot_asset_kernel_bin_end - boot_asset_kernel_bin_start);
}

#endif
