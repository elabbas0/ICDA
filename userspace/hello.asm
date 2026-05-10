bits 64
org 0

%define ICX_MAGIC         0x31584349
%define ICX_VERSION       1
%define SYS_CONSOLE_WRITE 0
%define SYS_EXIT          4

dd ICX_MAGIC
dw ICX_VERSION
dw header_end - $$
dq _start - $$
dq 0
header_end:

_start:
    lea rdi, [rel msg]
    mov eax, SYS_CONSOLE_WRITE
    int 0x80

    xor edi, edi
    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang

msg:
    db "hello from /bin/hello.icx", 10, 0
