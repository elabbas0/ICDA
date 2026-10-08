bits 64

; the vDSO image mapped into Linux programs (kernel/linux/vdso/vdso.c)
section .rodata
global lx_vdso_start
global lx_vdso_end
align 4096
lx_vdso_start:
    incbin "kernel/linux/vdso/vdso.so"
lx_vdso_end:

section .note.GNU-stack noalloc noexec nowrite progbits
