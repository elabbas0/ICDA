bits 64

%define GDT_USER_CODE 0x18
%define GDT_USER_DATA 0x20

global user_enter
extern user_return_rsp
extern user_return_rbx
extern user_return_rbp
extern user_return_r12
extern user_return_r13
extern user_return_r14
extern user_return_r15
user_enter:
    mov [rel user_return_rsp], rsp
    mov [rel user_return_rbx], rbx
    mov [rel user_return_rbp], rbp
    mov [rel user_return_r12], r12
    mov [rel user_return_r13], r13
    mov [rel user_return_r14], r14
    mov [rel user_return_r15], r15
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
