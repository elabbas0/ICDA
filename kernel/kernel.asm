bits 32

; multiboot2 header
section .multiboot
align 8
multiboot_start:
    dd 0xE85250D6
    dd 0
    dd multiboot_end - multiboot_start
    dd -(0xE85250D6 + 0 + (multiboot_end - multiboot_start))

    ; framebuffer request tag
    align 8
    dw 5
    dw 0
    dd 20
    dd 1024
    dd 768
    dd 32

    ; end tag
    align 8
    dw 0
    dw 0
    dd 8
multiboot_end:

; bss - page tables, multiboot pointer, stack
section .bss
align 4096
pml4_table:         resb 4096
pdp_table:          resb 4096
pd_table:           resb 4096

align 8
multiboot_info_ptr: resq 1

; stack must be last so page table setup doesn't overwrite it
align 16
stack_bottom:
    resb 16384
stack_top:

; gdt for 64-bit long mode
section .data
align 8
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

; entry point - 32-bit protected mode
section .text
global _start
global multiboot_info_ptr
extern kernel_main

_start:
    mov esp, stack_top
    mov [multiboot_info_ptr], ebx

    ; check long mode support
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ; set up paging tables
    mov eax, pdp_table
    or eax, 0b11
    mov [pml4_table], eax

    mov eax, pd_table
    or eax, 0b11
    mov [pdp_table], eax

    mov ecx, 0
.map_pd:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10000011
    mov [pd_table + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd

    ; enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; point cr3 to pml4
    mov eax, pml4_table
    mov cr3, eax

    ; enable long mode via EFER
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; enable paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; load GDT and far jump to 64-bit
    lgdt [gdt64.pointer]
    jmp gdt64.code:.long_mode

; 64-bit long mode
bits 64
.long_mode:
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; zero extend 32-bit multiboot ptr into rdi (first argument)
    xor rdi, rdi
    mov edi, [multiboot_info_ptr]
    call kernel_main
    hlt

; error - no long mode support
bits 32
.no_long_mode:
    mov dword [0xb8000], 0x4F4F4F4E
    mov dword [0xb8004], 0x4F34364F
    hlt

section .note.GNU-stack noalloc noexec nowrite progbits