bits 64
default rel
global _start
extern __libc_start
section .text
_start:
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    and rsp, -16
    call __libc_start
.hang:
    jmp .hang
section .note.GNU-stack noalloc noexec nowrite progbits
