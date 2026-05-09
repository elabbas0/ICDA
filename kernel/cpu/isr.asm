bits 64

; macro for exceptions WITHOUT error code
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0              ; dummy error code
    push %1             ; interrupt number
    jmp isr_common
%endmacro

; macro for exceptions WITH error code
%macro ISR_ERR 1
global isr%1
isr%1:
    push %1             ; interrupt number
    jmp isr_common
%endmacro

; macro for hardware irqs
%macro IRQ 2
global irq%1
irq%1:
    push 0              ; dummy error code
    push %2             ; interrupt number
    jmp irq_common
%endmacro

%macro SYSCALL 2
global %1
%1:
    push 0
    push %2
    jmp syscall_common
%endmacro

; cpu exceptions
ISR_NOERR 0   ; divide by zero
ISR_NOERR 1   ; debug
ISR_NOERR 2   ; non-maskable interrupt
ISR_NOERR 3   ; breakpoint
ISR_NOERR 4   ; overflow
ISR_NOERR 5   ; bound range exceeded
ISR_NOERR 6   ; invalid opcode
ISR_NOERR 7   ; device not available
ISR_ERR   8   ; double fault
ISR_NOERR 9   ; coprocessor segment overrun
ISR_ERR   10  ; invalid TSS
ISR_ERR   11  ; segment not present
ISR_ERR   12  ; stack-segment fault
ISR_ERR   13  ; general protection fault
ISR_ERR   14  ; page fault
ISR_NOERR 15  ; reserved
ISR_NOERR 16  ; x87 floating point
ISR_ERR   17  ; alignment check
ISR_NOERR 18  ; machine check
ISR_NOERR 19  ; SIMD floating point
ISR_NOERR 20  ; virtualization
ISR_NOERR 21  ; reserved
ISR_NOERR 22  ; reserved
ISR_NOERR 23  ; reserved
ISR_NOERR 24  ; reserved
ISR_NOERR 25  ; reserved
ISR_NOERR 26  ; reserved
ISR_NOERR 27  ; reserved
ISR_NOERR 28  ; reserved
ISR_NOERR 29  ; reserved
ISR_ERR   30  ; security exception
ISR_NOERR 31  ; reserved

; hardware irqs (irq number, interrupt number)
IRQ 0,  32    ; timer
IRQ 1,  33    ; keyboard
IRQ 2,  34    ; cascade
IRQ 3,  35    ; COM2
IRQ 4,  36    ; COM1
IRQ 5,  37    ; LPT2
IRQ 6,  38    ; floppy
IRQ 7,  39    ; LPT1
IRQ 8,  40    ; real time clock
IRQ 9,  41    ; free
IRQ 10, 42    ; free
IRQ 11, 43    ; free
IRQ 12, 44    ; PS/2 mouse
IRQ 13, 45    ; FPU
IRQ 14, 46    ; primary ATA
IRQ 15, 47    ; secondary ATA
SYSCALL syscall128, 128

; common exception handler
extern isr_handler
isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rdi, rsp        ; first arg = pointer to registers on stack
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16         ; skip int_no and err_code
    iretq

; common irq handler
extern irq_handler
irq_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rdi, rsp        ; first arg = pointer to registers on stack
    call irq_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16         ; skip int_no and err_code
    iretq

extern syscall_handler
syscall_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rdi, rsp
    call syscall_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16
    iretq

; idt flush
global idt_flush
idt_flush:
    lidt [rdi]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
