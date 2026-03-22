bits 32

; Multiboot2 header
section .multiboot
align 8
multiboot_start:
    dd 0xE85250D6               ; magic
    dd 0                        ; architecture (0 = x86)
    dd multiboot_end - multiboot_start
    dd -(0xE85250D6 + 0 + (multiboot_end - multiboot_start))
    ; end tag
    dw 0
    dw 0
    dd 8
multiboot_end:

; STACK 
section .bss
align 16
stack_bottom:
    resb 16384                  ; 16KB stack
stack_top:

; page tables (each 4KB)
align 4096
pml4_table:     resb 4096       ; Page Map Level 4
pdp_table:      resb 4096       ; Page Directory Pointer
pd_table:       resb 4096       ; Page Directory

; GDT FOR 64-BIT LONG MODE
section .data
align 8
gdt64:
    dq 0                        ; null descriptor
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)  ; 64-bit code segment
.pointer:
    dw $ - gdt64 - 1            ; GDT limit
    dq gdt64                    ; GDT base address

; 32-bit protected mode
section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top          ; set up stack

    ; cpu check for long mode
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29           ; long mode bit
    jz .no_long_mode

    ; setup paging tables
    ; PML4[0] → PDP
    mov eax, pdp_table
    or eax, 0b11                ; present + writable
    mov [pml4_table], eax

    ; PDP[0] → PD
    mov eax, pd_table
    or eax, 0b11                ; present + writable
    mov [pdp_table], eax

    ; PD — map 1GB using 2MB pages (512 entries)
    mov ecx, 0
.map_pd:
    mov eax, 0x200000           ; 2MB
    mul ecx
    or eax, 0b10000011          ; present + writable + huge page
    mov [pd_table + ecx * 8], eax
    inc ecx
    cmp ecx, 512
    jne .map_pd

    mov eax, cr4
    or eax, 1 << 5              ; PAE bit
    mov cr4, eax

    ; cr3 to pml4
    mov eax, pml4_table
    mov cr3, eax

    ; enable long mode
    mov ecx, 0xC0000080         ; EFER MSR address
    rdmsr
    or eax, 1 << 8              ; long mode enable bit
    wrmsr

    ; enable paging
    mov eax, cr0
    or eax, 1 << 31             ; paging bit
    mov cr0, eax

    ; gdt and far jump
    lgdt [gdt64.pointer]
    jmp gdt64.code:.long_mode   ; far jump flushes pipeline

bits 64
.long_mode:
    ; clear all segment registers
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call kernel_main            ; jump into C
    hlt                         ; halt if kernel_main returns

; Handle non 64
bits 32
.no_long_mode:
    ; print "NO64" to screen in red so we know what happened
    mov dword [0xb8000], 0x4F4F4F4E  ; "NO" red on white
    mov dword [0xb8004], 0x4F34364F  ; "64" red on white
    hlt