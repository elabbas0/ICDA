bits 64
default rel

global _start
extern init_main

section .text
_start:
    ; Standard argc/argv stack layout (see user_build_initial_stack):
    ; [rsp] = argc, [rsp+8] = argv[0].
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    call init_main
    mov rdi, rax
    mov eax, 4
    int 0x80
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
