bits 64
global idt_flush

idt_flush:
    lidt [rdi]      ; rdi = first argument (the idt_ptr address)
    ret