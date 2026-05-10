bits 64
default rel

global _start

section .text
_start:
    sub rsp, 64

    lea rdi, [prefix]
    mov eax, 0
    int 0x80

    mov eax, 1
    int 0x80

    mov rbx, rax
    lea rsi, [rsp + 31]
    mov byte [rsi], 0
    dec rsi
    mov byte [rsi], 10

    cmp rbx, 0
    jne .convert
    dec rsi
    mov byte [rsi], '0'
    jmp .print

.convert:
    mov rcx, 10
.loop:
    mov rax, rbx
    xor rdx, rdx
    div rcx
    add dl, '0'
    dec rsi
    mov [rsi], dl
    mov rbx, rax
    test rbx, rbx
    jne .loop

.print:
    mov rdi, rsi
    mov eax, 0
    int 0x80

    xor edi, edi
    mov eax, 4
    int 0x80

.hang:
    jmp .hang

section .rodata
prefix: db "pid: ", 0
