bits 64
default rel

global _start
extern shell_main

section .text
_start:
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    call shell_main
    mov rdi, rax
    mov eax, 4
    int 0x80
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
