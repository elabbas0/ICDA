bits 64

section .rodata
global usershell_start
global usershell_end

usershell_start:
    incbin "userspace/shell.app"
usershell_end:

section .note.GNU-stack noalloc noexec nowrite progbits
