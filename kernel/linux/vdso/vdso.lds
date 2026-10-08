/* one read + execute segment; the data page (vvar) is mapped right below */
SECTIONS {
    . = SIZEOF_HEADERS;
    .hash    : { *(.hash) }
    .dynsym  : { *(.dynsym) }
    .dynstr  : { *(.dynstr) }
    .text    : { *(.text*) }
    .rodata  : { *(.rodata*) }
    .dynamic : { *(.dynamic) }
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) *(.data*) *(.bss*) *(.got*) }
}
PROVIDE_HIDDEN(__vvar = -4096);
