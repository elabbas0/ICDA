bits 64


%macro ISR_NOERR 1
global isr%1
isr%1:
    push 0              
    push %1             
    jmp isr_common
%endmacro


%macro ISR_ERR 1
global isr%1
isr%1:
    push %1             
    jmp isr_common
%endmacro


%macro IRQ 2
global irq%1
irq%1:
    push 0              
    push %2             
    jmp irq_common
%endmacro

%macro SYSCALL 2
global %1
%1:
    push 0
    push %2
    jmp syscall_common
%endmacro


ISR_NOERR 0   
ISR_NOERR 1   
ISR_NOERR 2   
ISR_NOERR 3   
ISR_NOERR 4   
ISR_NOERR 5   
ISR_NOERR 6   
ISR_NOERR 7   
ISR_ERR   8   
ISR_NOERR 9   
ISR_ERR   10  
ISR_ERR   11  
ISR_ERR   12  
ISR_ERR   13  
ISR_ERR   14  
ISR_NOERR 15  
ISR_NOERR 16  
ISR_ERR   17  
ISR_NOERR 18  
ISR_NOERR 19  
ISR_NOERR 20  
ISR_NOERR 21  
ISR_NOERR 22  
ISR_NOERR 23  
ISR_NOERR 24  
ISR_NOERR 25  
ISR_NOERR 26  
ISR_NOERR 27  
ISR_NOERR 28  
ISR_NOERR 29  
ISR_ERR   30  
ISR_NOERR 31  


IRQ 0,  32    
IRQ 1,  33    
IRQ 2,  34    
IRQ 3,  35    
IRQ 4,  36    
IRQ 5,  37    
IRQ 6,  38    
IRQ 7,  39    
IRQ 8,  40    
IRQ 9,  41    
IRQ 10, 42    
IRQ 11, 43    
IRQ 12, 44    
IRQ 13, 45    
IRQ 14, 46    
IRQ 15, 47    
SYSCALL syscall128, 128

%define GDT_KERNEL_DATA 0x10
%define GDT_USER_RPL    0x3


extern isr_handler
isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, GDT_KERNEL_DATA
    mov ds, ax
    mov es, ax

    mov rdi, rsp        
    call isr_handler

    
    
    
    
    
    
    test byte [rsp + 18*8], GDT_USER_RPL
    jz .isr_return_kernel
    xor eax, eax
    jmp .isr_return_segs
.isr_return_kernel:
    mov eax, GDT_KERNEL_DATA
.isr_return_segs:
    mov ds, ax
    mov es, ax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16         
    iretq


extern irq_handler
irq_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, GDT_KERNEL_DATA
    mov ds, ax
    mov es, ax

    mov rdi, rsp        
    call irq_handler

    
    
    
    
    
    
    test byte [rsp + 18*8], GDT_USER_RPL
    jz .irq_return_kernel
    xor eax, eax
    jmp .irq_return_segs
.irq_return_kernel:
    mov eax, GDT_KERNEL_DATA
.irq_return_segs:
    mov ds, ax
    mov es, ax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16         
    iretq

extern syscall_handler
extern current_thread_ptr
extern user_thread_finish

%define THREAD_KERNEL_STACK_TOP 32
%define THREAD_USER_RETURN_RSP 72
%define THREAD_USER_RETURN_RBX 80
%define THREAD_USER_RETURN_RBP 88
%define THREAD_USER_RETURN_R12 96
%define THREAD_USER_RETURN_R13 104
%define THREAD_USER_RETURN_R14 112
%define THREAD_USER_RETURN_R15 120
%define THREAD_USER_RETURN_PENDING 128
syscall_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov ax, GDT_KERNEL_DATA
    mov ds, ax
    mov es, ax

    mov rdi, rsp
    call syscall_handler

    
    
    
    
    
    
    
    
    mov rax, [rsp + 14*8]
    mov r11, [rel current_thread_ptr]
    cmp qword [r11 + THREAD_USER_RETURN_PENDING], 0
    je .sysret_user
    mov qword [r11 + THREAD_USER_RETURN_PENDING], 0
    mov rsp, [r11 + THREAD_KERNEL_STACK_TOP]
    sub rsp, 8
    mov qword [rsp], 0
    mov ax, GDT_KERNEL_DATA
    mov ss, ax
    mov ds, ax
    mov es, ax
    jmp user_thread_finish
.sysret_user:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    add rsp, 8

    add rsp, 16
    xor ecx, ecx
    mov ds, cx
    mov es, cx
    iretq


global linux_syscall_entry
extern syscall_kstack_top
extern syscall_user_rsp

linux_syscall_entry:
    mov [rel syscall_user_rsp], rsp
    mov rsp, [rel syscall_kstack_top]
    push qword 0x23
    push qword [rel syscall_user_rsp]
    push r11
    push qword 0x1B
    push rcx
    push 0
    push 128
    jmp syscall_common

global lx_resume_user
lx_resume_user:
    mov rsp, rdi
    xor eax, eax
    mov ds, ax
    mov es, ax
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq

global idt_flush
idt_flush:
    lidt [rdi]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
