# Multiboot 1 shim that loads ICDA's Multiboot 2 kernel (see shim.c)
CC ?= gcc
CFLAGS = -m32 -ffreestanding -fno-pie -fno-stack-protector -fno-builtin \
         -mno-sse -mno-mmx -O2 -Wall -Wextra -nostdlib

shim.elf: shim.c start.S shim.ld
	$(CC) $(CFLAGS) -no-pie -Wl,-T,shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -o $@ start.S shim.c

clean:
	rm -f shim.elf

.PHONY: clean
