bits 32

; MULTIBOOT2 HEADER
section .multiboot
align 8
multiboot_start:
    dd 0xE85250D6                                           ; magic
    dd 0                                                    ; architecture
    dd multiboot_end - multiboot_start                      ; header length
    dd -(0xE85250D6 + 0 + (multiboot_end - multiboot_start)); checksum
 
    ; --- framebuffer request tag ---
    align 8
    dw 5                    ; type 5 = framebuffer
    dw 0                    ; flags
    dd 20                   ; size (2+2+4+4+4+4 = 20 bytes)
    dd 1024                 ; width
    dd 768                  ; height
    dd 32                   ; bits per pixel
 
    ; --- end tag (must be last, must be 8-byte aligned) ---
    align 8
    dw 0                    ; type 0 = end
    dw 0                    ; flags
    dd 8                    ; size
multiboot_end:

; STACK
section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

; page tables
align 4096
pml4_table:     resb 4096
pdp_table:      resb 4096
pd_table:       resb 4096


; GDT FOR 64-BIT LONG MODE
section .data
align 8
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

; 32-bit protected mode
section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top

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

    ; load GDT and jump to 64-bit
    lgdt [gdt64.pointer]
    jmp gdt64.code:.long_mode


bits 64
.long_mode:
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call kernel_main
    hlt

bits 32
.no_long_mode:
    mov dword [0xb8000], 0x4F4F4F4E
    mov dword [0xb8004], 0x4F34364F
    hlt

section .note.GNU-stack noalloc noexec nowrite progbits