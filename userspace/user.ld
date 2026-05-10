ENTRY(_start)

SECTIONS {
    . = 0x0000008000400000;

    .text : ALIGN(4K) {
        *(.text*)
        *(.rodata*)
    }

    .data : ALIGN(4K) {
        *(.data*)
    }

    .bss : ALIGN(4K) {
        *(.bss*)
        *(COMMON)
    }
}
