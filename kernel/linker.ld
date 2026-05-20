OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x100000;

    .text BLOCK(4K) : ALIGN(4K) {
        *(.multiboot)
        *(.text)
    }

    .dma_low BLOCK(4K) : ALIGN(4K) {
        *(.dma_low)
    }

    .rodata BLOCK(4K) : ALIGN(4K) {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K) {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K) {
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
