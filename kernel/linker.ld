OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* The kernel runs in the higher half (KERNEL_VMA + physical address) so the
 * whole lower half of every address space belongs to user programs; static
 * Linux binaries load at 0x400000, which used to overlap the kernel image.
 * Only the multiboot header and the 32-bit bootstrap (.boot*) stay low. */
KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS {
    . = 0x100000;
    kernel_start = .;

    .boot : {
        *(.multiboot)
        *(.boot_text)
        *(.boot_data)
    }

    .boot_bss ALIGN(4K) (NOLOAD) : {
        *(.boot_bss)
    }

    . = ALIGN(4K) + KERNEL_VMA;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        *(.text)
    }

    .dma_low ALIGN(4K) : AT(ADDR(.dma_low) - KERNEL_VMA) {
        *(.dma_low)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        *(COMMON)
        *(.bss)
    }

    kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note*)
        *(.ctors)
        *(.dtors)
        *(.gcc_except_table)
    }
}
