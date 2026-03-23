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
; map 0-64GB using one PML4 entry -> one PDP -> 64 page directories
; each page directory covers 1GB using 512 x 2MB huge pages
section .bss
align 4096
pml4_table:     resb 4096
pdp_table:      resb 4096

; 64 page directories = 64GB total coverage
pd_table_0:     resb 4096
pd_table_1:     resb 4096
pd_table_2:     resb 4096
pd_table_3:     resb 4096
pd_table_4:     resb 4096
pd_table_5:     resb 4096
pd_table_6:     resb 4096
pd_table_7:     resb 4096
pd_table_8:     resb 4096
pd_table_9:     resb 4096
pd_table_10:    resb 4096
pd_table_11:    resb 4096
pd_table_12:    resb 4096
pd_table_13:    resb 4096
pd_table_14:    resb 4096
pd_table_15:    resb 4096
pd_table_16:    resb 4096
pd_table_17:    resb 4096
pd_table_18:    resb 4096
pd_table_19:    resb 4096
pd_table_20:    resb 4096
pd_table_21:    resb 4096
pd_table_22:    resb 4096
pd_table_23:    resb 4096
pd_table_24:    resb 4096
pd_table_25:    resb 4096
pd_table_26:    resb 4096
pd_table_27:    resb 4096
pd_table_28:    resb 4096
pd_table_29:    resb 4096
pd_table_30:    resb 4096
pd_table_31:    resb 4096
pd_table_32:    resb 4096
pd_table_33:    resb 4096
pd_table_34:    resb 4096
pd_table_35:    resb 4096
pd_table_36:    resb 4096
pd_table_37:    resb 4096
pd_table_38:    resb 4096
pd_table_39:    resb 4096
pd_table_40:    resb 4096
pd_table_41:    resb 4096
pd_table_42:    resb 4096
pd_table_43:    resb 4096
pd_table_44:    resb 4096
pd_table_45:    resb 4096
pd_table_46:    resb 4096
pd_table_47:    resb 4096
pd_table_48:    resb 4096
pd_table_49:    resb 4096
pd_table_50:    resb 4096
pd_table_51:    resb 4096
pd_table_52:    resb 4096
pd_table_53:    resb 4096
pd_table_54:    resb 4096
pd_table_55:    resb 4096
pd_table_56:    resb 4096
pd_table_57:    resb 4096
pd_table_58:    resb 4096
pd_table_59:    resb 4096
pd_table_60:    resb 4096
pd_table_61:    resb 4096
pd_table_62:    resb 4096
pd_table_63:    resb 4096

align 8
multiboot_info_ptr: resq 1

; stack must be last
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

section .text
global _start
global multiboot_info_ptr
extern kernel_main

; macro: fill one page directory with 512 x 2MB pages
; %1 = page directory address
; %2 = base physical address in GB (e.g. 0 = 0GB, 1 = 1GB)
%macro map_pd 2
    mov edi, %1
    mov eax, (%2 * 0x40000000)
    or eax, 0b10000011
    mov ecx, 512
%%loop:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop %%loop
%endmacro

; entry point - 32-bit protected mode
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

    ; wire PML4[0] -> PDP
    mov eax, pdp_table
    or eax, 0b11
    mov [pml4_table], eax

    ; wire PDP[0..3] -> first 4 page directories (0-4GB, done in 32-bit)
    mov eax, pd_table_0
    or eax, 0b11
    mov [pdp_table + 0 * 8], eax

    mov eax, pd_table_1
    or eax, 0b11
    mov [pdp_table + 1 * 8], eax

    mov eax, pd_table_2
    or eax, 0b11
    mov [pdp_table + 2 * 8], eax

    mov eax, pd_table_3
    or eax, 0b11
    mov [pdp_table + 3 * 8], eax

    ; map first 4GB (32-bit can handle these addresses)
    map_pd pd_table_0, 0
    map_pd pd_table_1, 1
    map_pd pd_table_2, 2
    map_pd pd_table_3, 3

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

; 64-bit long mode - now we can use full 64-bit addresses
bits 64
.long_mode:
    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; map 4GB-64GB using 64-bit addresses
    ; each PDP entry covers 1GB, we fill entries 4-63
    ; pd_table_0 is contiguous in memory so we can walk through them
    ; pd_table_N = pd_table_0 + N * 4096
    mov rcx, 4                         ; start at PDP entry 4 (4GB)
.map_high:
    ; calculate pd_table address: pd_table_0 + rcx * 4096
    mov rax, pd_table_0
    mov rbx, rcx
    shl rbx, 12                        ; rbx = rcx * 4096
    add rax, rbx                       ; rax = address of pd_table_N
    or rax, 0b11
    mov [pdp_table + rcx * 8], rax     ; PDP[rcx] -> pd_table_N

    ; fill this page directory: 512 x 2MB pages starting at rcx * 1GB
    mov r8, pd_table_0
    add r8, rbx                        ; r8 = pd_table_N address
    mov r9, rcx
    shl r9, 30                         ; r9 = rcx * 1GB (base address)
    or r9, 0b10000011                  ; present + writable + huge
    mov r10, 0                         ; entry index
.fill_pd:
    mov [r8 + r10 * 8], r9
    add r9, 0x200000                   ; next 2MB
    inc r10
    cmp r10, 512
    jne .fill_pd

    inc rcx
    cmp rcx, 64                        ; map up to 64GB
    jne .map_high

    ; reload cr3 to flush TLB with new mappings
    mov rax, pml4_table
    mov cr3, rax

    ; pass multiboot info pointer as first argument (rdi)
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