bits 64

; macro for exceptions WITHOUT error code
; cpu doesn't push one so we push a dummy 0
%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0              ; dummy error code
    push %1             ; interrupt number
    jmp isr_common
%endmacro

; macro for exceptions WITH error code
; cpu already pushed one so we just push the number
%macro ISR_ERR 1
global isr%1
isr%1:
    push %1             ; interrupt number
    jmp isr_common
%endmacro

; macro for hardware irqs
; none have error codes
%macro IRQ 2
global irq%1
irq%1:
    push 0              ; dummy error code
    push %2             ; interrupt number
    jmp irq_common
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
ISR_ERR   8   ; double fault (has error code)
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

; common exception handler
; saves all registers, calls isr_handler(regs*)
extern isr_handler
isr_common:
    ; save general purpose registers
    push rax
    push rcx
    push rdx
    push rbx
    push rsp
    push rbp
    push rsi
    push rdi

    ; save data segment
    mov ax, ds
    push rax

    ; load kernel data segment
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; call C handler with pointer to saved registers
    mov rdi, rsp
    call isr_handler

    ; restore data segment
    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; restore registers
    pop rdi
    pop rsi
    pop rbp
    pop rsp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    ; clean up pushed error code and interrupt number
    add rsp, 16
    iretq

; common irq handler
extern irq_handler
irq_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rsp
    push rbp
    push rsi
    push rdi

    mov ax, ds
    push rax

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rdi, rsp
    call irq_handler

    pop rax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    pop rdi
    pop rsi
    pop rbp
    pop rsp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16
    iretq

; idt flush - loads the IDT pointer
global idt_flush
idt_flush:
    lidt [rdi]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits