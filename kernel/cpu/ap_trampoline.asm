; Application processor start-up code, assembled as a flat binary and copied
; to physical 0x8000.  The SIPI starts each AP here in real mode; it goes
; through protected mode into long mode on the kernel's page tables and calls
; ap_entry(cpu) on the stack the BSP prepared.  The BSP fills the parameter
; block at offset 8 before sending the SIPI.

bits 16
org 0x8000

ap_start:
    jmp short real_start
    align 8
param_cr3:   dq 0       ; +8
param_cr4:   dq 0       ; +16
param_cr0:   dq 0       ; +24
param_stack: dq 0       ; +32
param_cpu:   dq 0       ; +40
param_entry: dq 0       ; +48
param_efer:  dq 0       ; +56

    align 8
gdt32:
    dq 0
    dq 0x00CF9A000000FFFF   ; 0x08 32-bit code
    dq 0x00CF92000000FFFF   ; 0x10 data
    dq 0x00AF9A000000FFFF   ; 0x18 64-bit code
gdt32_ptr:
    dw gdt32_ptr - gdt32 - 1
    dd gdt32

real_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [gdt32_ptr]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm32

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, [param_cr4]
    mov cr4, eax
    mov eax, [param_cr3]
    mov cr3, eax
    mov ecx, 0xC0000080
    mov eax, [param_efer]
    mov edx, [param_efer + 4]
    wrmsr
    mov eax, [param_cr0]
    mov cr0, eax
    jmp 0x18:lm64

bits 64
lm64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [param_stack]
    mov rdi, [param_cpu]
    mov rax, [param_entry]
    call rax
.hang:
    cli
    hlt
    jmp .hang
