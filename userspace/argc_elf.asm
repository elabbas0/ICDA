bits 64
default rel

global _start

section .text
_start:
    mov rbx, [rsp]           ; argc

    mov eax, 1               ; linux write
    mov edi, 1
    lea rsi, [msg_argc]
    mov rdx, msg_argc_len
    int 0x80

    mov rax, rbx
    call print_uint_nl

    cmp rbx, 1
    jbe .done

    mov eax, 1
    mov edi, 1
    lea rsi, [msg_arg1]
    mov rdx, msg_arg1_len
    int 0x80

    mov rsi, [rsp + 16]      ; argv[1]
    xor rdx, rdx
.len_loop:
    cmp byte [rsi + rdx], 0
    je .have_len
    inc rdx
    jmp .len_loop
.have_len:
    mov eax, 1
    mov edi, 1
    int 0x80

    lea rsi, [nl]
    mov eax, 1
    mov edi, 1
    mov rdx, 1
    int 0x80

.done:
    mov edi, ebx
    mov eax, 60              ; linux exit
    int 0x80

print_uint_nl:
    sub rsp, 40
    lea rsi, [rsp + 39]
    mov byte [rsi], 0
    dec rsi
    mov byte [rsi], 10
    cmp rax, 0
    jne .conv
    dec rsi
    mov byte [rsi], '0'
    jmp .emit
.conv:
    mov rcx, 10
.loop:
    xor rdx, rdx
    div rcx
    add dl, '0'
    dec rsi
    mov [rsi], dl
    test rax, rax
    jne .loop
.emit:
    mov rdx, rsp
    add rdx, 40
    sub rdx, rsi
    mov eax, 1
    mov edi, 1
    int 0x80
    add rsp, 40
    ret

section .rodata
msg_argc: db "argc=", 0
msg_argc_len equ $ - msg_argc - 1
msg_arg1: db "argv1=", 0
msg_arg1_len equ $ - msg_arg1 - 1
nl: db 10
