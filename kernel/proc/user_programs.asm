bits 64

section .rodata
global userprog_hello_start
global userprog_hello_end
global userprog_pid_start
global userprog_pid_end
global userprog_hello_elf_start
global userprog_hello_elf_end
global userprog_pid_elf_start
global userprog_pid_elf_end

userprog_hello_start:
    incbin "userspace/hello.icx"
userprog_hello_end:

userprog_pid_start:
    incbin "userspace/pid.icx"
userprog_pid_end:

userprog_hello_elf_start:
    incbin "userspace/hello.elf"
userprog_hello_elf_end:

userprog_pid_elf_start:
    incbin "userspace/pid.elf"
userprog_pid_elf_end:

section .note.GNU-stack noalloc noexec nowrite progbits
