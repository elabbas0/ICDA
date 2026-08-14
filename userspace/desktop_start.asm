bits 64
default rel

global _start
extern desktop_main

section .text
_start:
    call desktop_main
    mov rdi, rax
    mov eax, 4
    int 0x80
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
