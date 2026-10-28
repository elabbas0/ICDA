/* Linked well above ICDA's own image (0x100000 - ~0x3000000) so copying the
 * kernel never overwrites the shim; QEMU places the module right after it. */
ENTRY(_start)
SECTIONS
{
    . = 0x7000000;
    .text : { *(.multiboot) *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
