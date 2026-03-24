bits 64

; void switch_context(process_t *prev, process_t *next)
;                       rdi               rsi
;
; process_t layout (must match process.h exactly):
;   offset 0 : kernel_rsp  (uint64_t)   ← only field we touch
;
; What we save/restore: the 6 callee-saved registers defined by the
; System V AMD64 ABI (rbx, rbp, r12, r13, r14, r15).
; RIP is implicitly saved by the call instruction (return address on stack)
; and restored by the final `ret`.
;
; Stack frame in a suspended process (top → bottom, lower addresses first):
;   [rsp+0 ] ← r15
;   [rsp+8 ] ← r14
;   [rsp+16] ← r13
;   [rsp+24] ← r12
;   [rsp+32] ← rbp
;   [rsp+40] ← rbx
;   [rsp+48] ← return address (rip to resume at)

global switch_context
switch_context:
    ; ── save prev 
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    mov     [rdi], rsp          ; prev->kernel_rsp = rsp

    ; ── restore next 
    mov     rsp, [rsi]          ; rsp = next->kernel_rsp

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx

    ret                         ; jump to next->saved rip

section .note.GNU-stack noalloc noexec nowrite progbits
