ICDA SDK
========

Build static native programs for ICDA with the host gcc and ld (x86_64).

  include/   C headers: stdio.h stdlib.h string.h ctype.h, plus icda_sys.h for
             raw ICDA system calls
  lib/       crt1.o (entry point), libc.o (C library and malloc), user.ld

  make            builds hello.elf from hello.c
  make prog.elf   builds prog.elf from prog.c

The link step writes 0xFF into the ELF OSABI byte. That marks the binary as a
native ICDA program, so it runs with the native ABI even from /bin (other ELFs
in /bin run as Linux programs). Copy the .elf onto a volume ICDA mounts, or
fetch it with curl, then run it from the Terminal by its path.
