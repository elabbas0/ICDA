bits 64
default rel

global _start
extern nptestlx_main

section .text
_start:
    ; Linux x86-64 entry convention, matching user_build_initial_stack:
    ; [rsp] = argc, [rsp+8] = argv[0].
    mov rdi, [rsp]
    lea rsi, [rsp + 8]
    call nptestlx_main
    ; In /bin/ this process has the Linux personality, so exit via
    ; Linux nr 60 (native SYS_EXIT would hit the default -1 path).
    mov rdi, rax
    mov eax, 60
    int 0x80
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
