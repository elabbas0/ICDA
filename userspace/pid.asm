bits 64
org 0

%define ICX_MAGIC         0x31584349
%define ICX_VERSION       1
%define SYS_CONSOLE_WRITE 0
%define SYS_GET_PID       1
%define SYS_EXIT          4

dd ICX_MAGIC
dw ICX_VERSION
dw header_end - $$
dq _start - $$
dq 0
header_end:

_start:
    sub rsp, 64

    lea rdi, [rel prefix]
    mov eax, SYS_CONSOLE_WRITE
    int 0x80

    mov eax, SYS_GET_PID
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
    mov eax, SYS_CONSOLE_WRITE
    int 0x80

    xor edi, edi
    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang

prefix:
    db "pid: ", 0
