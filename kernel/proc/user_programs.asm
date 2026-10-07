; SLIM=1 (installed kernel): only init is embedded; the other programs are
; system files on the ICDA partition (\SYSTEM), updated by OTA patches.
%ifndef SLIM
%define SLIM 0
%endif

bits 64




%ifndef CI_IMAGE
%define CI_IMAGE 0
%endif

section .rodata
global userprog_hello_start
global userprog_hello_end
global userprog_pid_start
global userprog_pid_end
global userprog_hello_elf_start
global userprog_hello_elf_end
global userprog_pid_elf_start
global userprog_pid_elf_end
global userprog_argc_elf_start
global userprog_argc_elf_end
global userprog_libctest_elf_start
global userprog_libctest_elf_end
global userprog_busybox_start
global userprog_busybox_end
global userprog_fetch_start
global userprog_fetch_end
global userprog_updated_start
global userprog_updated_end
global userprog_cacert_start
global userprog_cacert_end
global userprog_ticker_start
global userprog_ticker_end
global userprog_media_start
global userprog_media_end
global userprog_editor_start
global userprog_editor_end
global userprog_diskman_start
global userprog_diskman_end
global userprog_curl_start
global userprog_curl_end
global userprog_wm_start
global userprog_wm_end
global userprog_desktop_start
global userprog_desktop_end
global userprog_terminal_start
global userprog_terminal_end
%if CI_IMAGE != 0
global userprog_gui_demo_start
global userprog_gui_demo_end
%endif
global userprog_taskman_start
global userprog_taskman_end
global userprog_browser_start
global userprog_browser_end
global userprog_settings_start
global userprog_settings_end
%if CI_IMAGE != 0
global userprog_nptest_start
global userprog_nptest_end
global userprog_nptestlx_start
global userprog_nptestlx_end
%endif
global userprog_init_start
global userprog_init_end

userprog_hello_start:
%if SLIM == 0
    incbin "userspace/hello.icx"
%endif
userprog_hello_end:

userprog_pid_start:
%if SLIM == 0
    incbin "userspace/pid.icx"
%endif
userprog_pid_end:

userprog_hello_elf_start:
%if SLIM == 0
    incbin "userspace/hello.elf"
%endif
userprog_hello_elf_end:

userprog_pid_elf_start:
%if SLIM == 0
    incbin "userspace/pid.elf"
%endif
userprog_pid_elf_end:

userprog_argc_elf_start:
%if SLIM == 0
    incbin "userspace/argc.elf"
%endif
userprog_argc_elf_end:

userprog_libctest_elf_start:
%if SLIM == 0
    incbin "userspace/libctest.elf"
%endif
userprog_libctest_elf_end:

userprog_busybox_start:
%if SLIM == 0
    incbin "resources/linux/busybox"
%endif
userprog_busybox_end:

userprog_fetch_start:
%if SLIM == 0
    incbin "userspace/fetch.elf"
%endif
userprog_fetch_end:

userprog_updated_start:
%if SLIM == 0
    incbin "userspace/updated.elf"
%endif
userprog_updated_end:

userprog_cacert_start:
%if SLIM == 0
    incbin "resources/ssl/cacert.pem"
%endif
userprog_cacert_end:

userprog_ticker_start:
%if SLIM == 0
    incbin "userspace/ticker.icx"
%endif
userprog_ticker_end:

userprog_media_start:
%if SLIM == 0
    incbin "userspace/media.app"
%endif
userprog_media_end:

userprog_editor_start:
%if SLIM == 0
    incbin "userspace/editor.app"
%endif
userprog_editor_end:

userprog_diskman_start:
%if SLIM == 0
    incbin "userspace/diskman.app"
%endif
userprog_diskman_end:

userprog_curl_start:
%if SLIM == 0
    incbin "userspace/curl.app"
%endif
userprog_curl_end:

userprog_wm_start:
%if SLIM == 0
    incbin "userspace/wm.app"
%endif
userprog_wm_end:

userprog_desktop_start:
%if SLIM == 0
    incbin "userspace/desktop.app"
%endif
userprog_desktop_end:

userprog_terminal_start:
%if SLIM == 0
    incbin "userspace/terminal.app"
%endif
userprog_terminal_end:

%if CI_IMAGE != 0
userprog_gui_demo_start:
%if SLIM == 0
    incbin "userspace/gui_demo.app"
%endif
userprog_gui_demo_end:
%endif

userprog_taskman_start:
%if SLIM == 0
    incbin "userspace/taskman.app"
%endif
userprog_taskman_end:

userprog_browser_start:
%if SLIM == 0
    incbin "userspace/browser.app"
%endif
userprog_browser_end:

userprog_settings_start:
%if SLIM == 0
    incbin "userspace/settings.app"
%endif
userprog_settings_end:

%if CI_IMAGE != 0
userprog_nptest_start:
%if SLIM == 0
    incbin "userspace/nptest.app"
%endif
userprog_nptest_end:

userprog_nptestlx_start:
%if SLIM == 0
    incbin "userspace/nptestlx.elf"
%endif
userprog_nptestlx_end:
%endif

userprog_init_start:
    incbin "userspace/init.app"
userprog_init_end:

section .note.GNU-stack noalloc noexec nowrite progbits
