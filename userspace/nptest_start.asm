bits 64
default rel

global _start
extern nptest_main

section .text
_start:
    call nptest_main
    mov rdi, rax
    mov eax, 4
    int 0x80
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
