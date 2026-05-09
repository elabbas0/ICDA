bits 64

%define GDT_USER_CODE 0x18
%define GDT_USER_DATA 0x20

global user_enter
user_enter:
    cli
    mov ax, GDT_USER_DATA | 3
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword (GDT_USER_DATA | 3)
    push rsi
    pushfq
    push qword (GDT_USER_CODE | 3)
    push rdi
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
