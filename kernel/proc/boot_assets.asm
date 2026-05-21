bits 64

section .rodata
global boot_asset_efi_start
global boot_asset_efi_end
global boot_asset_grub_cfg_start
global boot_asset_grub_cfg_end
global boot_asset_kernel_bin_start
global boot_asset_kernel_bin_end

boot_asset_efi_start:
    incbin "kernel/fs/bootx64-install.efi"
boot_asset_efi_end:

boot_asset_grub_cfg_start:
    incbin "boot/grub/grub-install.cfg"
boot_asset_grub_cfg_end:

boot_asset_kernel_bin_start:
    incbin "kernel/install-kernel.bin"
boot_asset_kernel_bin_end:

section .note.GNU-stack noalloc noexec nowrite progbits
