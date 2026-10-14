/* one read + execute segment; the data page (vvar) is mapped right below */
PHDRS {
    image PT_LOAD FILEHDR PHDRS FLAGS(5);
    dynamic PT_DYNAMIC FLAGS(4);
}
SECTIONS {
    . = SIZEOF_HEADERS;
    .hash    : { *(.hash) } :image
    .dynsym  : { *(.dynsym) } :image
    .dynstr  : { *(.dynstr) } :image
    .text    : { *(.text*) } :image
    .rodata  : { *(.rodata*) } :image
    /* glibc relocates dynamic entries in place when PT_DYNAMIC says they
     * are writable. The shared vDSO page is read-only in every process. */
    .dynamic : { *(.dynamic) } :image :dynamic
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) *(.data*) *(.bss*) *(.got*) }
}
PROVIDE_HIDDEN(__vvar = -4096);
