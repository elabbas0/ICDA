bits 64

%define GDT_USER_CODE 0x18
%define GDT_USER_DATA 0x20

global user_enter
extern current_thread_ptr

%define THREAD_USER_RETURN_RSP 72
%define THREAD_USER_RETURN_RBX 80
%define THREAD_USER_RETURN_RBP 88
%define THREAD_USER_RETURN_R12 96
%define THREAD_USER_RETURN_R13 104
%define THREAD_USER_RETURN_R14 112
%define THREAD_USER_RETURN_R15 120
user_enter:
    mov rax, [rel current_thread_ptr]
    mov [rax + THREAD_USER_RETURN_RSP], rsp
    mov [rax + THREAD_USER_RETURN_RBX], rbx
    mov [rax + THREAD_USER_RETURN_RBP], rbp
    mov [rax + THREAD_USER_RETURN_R12], r12
    mov [rax + THREAD_USER_RETURN_R13], r13
    mov [rax + THREAD_USER_RETURN_R14], r14
    mov [rax + THREAD_USER_RETURN_R15], r15
    cli
    mov ax, GDT_USER_DATA | 3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword (GDT_USER_DATA | 3)
    push rsi
    pushfq
    or qword [rsp], 0x200
    push qword (GDT_USER_CODE | 3)
    push rdi
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
