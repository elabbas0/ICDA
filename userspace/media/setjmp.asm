; setjmp / longjmp (System V x86-64): saves the callee-saved registers,
; the stack pointer and the return address.
bits 64
section .text
global icda_setjmp
global icda_longjmp

icda_setjmp:
    mov [rdi], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rdx, [rsp + 8]          ; stack pointer after returning
    mov [rdi + 48], rdx
    mov rdx, [rsp]              ; return address
    mov [rdi + 56], rdx
    xor eax, eax
    ret

icda_longjmp:
    mov eax, esi
    test eax, eax
    jnz .nonzero
    inc eax                     ; longjmp(env, 0) returns 1
.nonzero:
    mov rbx, [rdi]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    jmp [rdi + 56]
