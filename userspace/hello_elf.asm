bits 64
default rel

global _start

section .text
_start:
    lea rdi, [msg]
    mov eax, 0
    int 0x80

    xor edi, edi
    mov eax, 4
    int 0x80

.hang:
    jmp .hang

section .rodata
msg: db "hello from /bin/hello.elf", 10, 0
