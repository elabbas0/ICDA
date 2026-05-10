bits 64
org 0

%define ICX_MAGIC         0x31584349
%define ICX_VERSION       1
%define SYS_CONSOLE_WRITE 0
%define SYS_GET_PID       1
%define SYS_EXIT          4
%define SYS_YIELD         18

dd ICX_MAGIC
dw ICX_VERSION
dw header_end - $$
dq _start - $$
dq 0
header_end:

_start:
    sub rsp, 64
    mov eax, SYS_GET_PID
    int 0x80
    mov ebx, eax

    mov ecx, 5
.loop:
    lea rdi, [rel prefix]
    mov eax, SYS_CONSOLE_WRITE
    int 0x80

    mov eax, ebx
    call print_uint

    lea rdi, [rel mid]
    mov eax, SYS_CONSOLE_WRITE
    int 0x80

    mov eax, 6
    sub eax, ecx
    call print_uint

    lea rdi, [rel nl]
    mov eax, SYS_CONSOLE_WRITE
    int 0x80

    mov eax, SYS_YIELD
    int 0x80

    loop .loop

    xor edi, edi
    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang

print_uint:
    push rbx
    push rcx
    push rdx
    push rsi
    push r8
    sub rsp, 32

    mov eax, eax
    mov r8, rsp
    lea rsi, [r8 + 32]
    mov byte [rsi - 1], 0
    mov rcx, 10

    cmp rax, 0
    jne .convert
    mov byte [rsi - 2], '0'
    lea rdi, [rsi - 2]
    jmp .emit

.convert:
    mov rbx, rsi
.digit_loop:
    xor rdx, rdx
    div rcx
    add dl, '0'
    dec rbx
    mov [rbx], dl
    test rax, rax
    jne .digit_loop
    mov rdi, rbx

.emit:
    mov eax, SYS_CONSOLE_WRITE
    int 0x80

    add rsp, 32
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    ret

prefix: db "ticker pid=", 0
mid:    db " step=", 0
nl:     db 10, 0
