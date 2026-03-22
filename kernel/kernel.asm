; Multiboot2 header , required for grub
section .multiboot
align 8
    dd 0xE85250D6          ; multiboot2 magic number
    dd 0                   ; (0 = x86)
    dd 24                  ; header length
    dd -(0xE85250D6 + 0 + 24) ; checksum

; create a basic stack
section .bss
align 16
stack_bottom:
    resb 16384             ; 16KB stack space
stack_top:

; entry point
section .text
global _start
extern kernel_main         ; execute kernel_main

_start:
    mov esp, stack_top     ; set up stack
    call kernel_main       ; jump into C 
    hlt                    