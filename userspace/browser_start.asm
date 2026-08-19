bits 64
default rel

global _start
extern main

section .text
_start:
    xor rbp, rbp
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    and rsp, -16
    call main
    mov rdi, rax
    mov eax, 4
    int 0x80
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits