bits 64

%define SYS_CONSOLE_WRITE 0
%define SYS_GET_PID       1
%define SYS_EXIT          4

global user_demo_start
global user_demo_end

section .rodata
user_demo_start:
    lea rdi, [rel msg]
    mov eax, SYS_CONSOLE_WRITE
    int 0x80

    mov eax, SYS_GET_PID
    int 0x80

    lea rdi, [rel done]
    mov eax, SYS_CONSOLE_WRITE
    int 0x80

    xor edi, edi
    mov eax, SYS_EXIT
    int 0x80

.hang:
    jmp .hang

msg:
    db "hello from ring 3", 10, 0
done:
    db "user program returned through syscall exit", 10, 0
user_demo_end:

section .note.GNU-stack noalloc noexec nowrite progbits
