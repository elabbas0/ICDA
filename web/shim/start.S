/* Entry from QEMU's Multiboot 1 loader: 32-bit protected mode, paging off,
 * eax = magic, ebx = Multiboot 1 information. */
    .section .note.GNU-stack,"",@progbits
    .section .text
    .global _start
_start:
    cli
    mov $stack_top, %esp
    push %ebx
    push %eax
    call shim_main
1:  hlt
    jmp 1b

    .section .bss
    .align 16
    .space 16384
stack_top:
