bits 64
default rel

; Shared userland entry point.
; The kernel places argc at [rsp] and argv at [rsp+8] on the initial
; stack (see user_build_initial_stack in kernel/proc/user.c). We pass
; both to main() and exit with main's return value.

global _start
extern main

section .text
_start:
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp + 8]      ; argv
    and rsp, -16            ; 16-byte align for the ABI
    call main
    mov rdi, rax            ; exit code
    mov eax, 4              ; SYS_EXIT
    int 0x80
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
