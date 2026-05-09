bits 64

; void switch_context(thread_t *prev, thread_t *next)
;                      rdi              rsi
;
; thread_t layout:
;   offset 0 : kernel_rsp

global switch_context
switch_context:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    mov     [rdi], rsp

    mov     rsp, [rsi]

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx

    ret

section .note.GNU-stack noalloc noexec nowrite progbits
