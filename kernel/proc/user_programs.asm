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
global userprog_ticker_start
global userprog_ticker_end
global userprog_audioplay_start
global userprog_audioplay_end
global userprog_editor_start
global userprog_editor_end
global userprog_diskman_start
global userprog_diskman_end
global userprog_curl_start
global userprog_curl_end

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

userprog_ticker_start:
    incbin "userspace/ticker.icx"
userprog_ticker_end:

userprog_audioplay_start:
    incbin "userspace/audioplay.app"
userprog_audioplay_end:

userprog_editor_start:
    incbin "userspace/editor.app"
userprog_editor_end:

userprog_diskman_start:
    incbin "userspace/diskman.app"
userprog_diskman_end:

userprog_curl_start:
    incbin "userspace/curl.app"
userprog_curl_end:

section .note.GNU-stack noalloc noexec nowrite progbits
