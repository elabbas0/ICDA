bits 64

global gdt_flush

; rdi = pointer to gdt_ptr struct
gdt_flush:
    lgdt [rdi]          ; load new GDT

    ; reload code segment via far return
    push 0x08           ; kernel code selector
    lea rax, [rel .done]
    push rax
    retfq               ; far return reloads CS

.done:
    ; reload all data segment registers
    mov ax, 0x10        ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits