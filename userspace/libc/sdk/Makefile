SDK ?= .
CC = gcc
CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-pie -no-pie -mcmodel=large \
         -fno-asynchronous-unwind-tables -fno-stack-protector -msse2 -mfpmath=sse \
         -nostdinc -isystem $(shell $(CC) -print-file-name=include) -I$(SDK)/include

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.elf: %.o
	ld -nostdlib -static -T $(SDK)/lib/user.ld -o $@ $(SDK)/lib/crt1.o $< $(SDK)/lib/libc.o
	printf '\377' | dd of=$@ bs=1 seek=7 conv=notrunc status=none

all: hello.elf

clean:
	rm -f *.o *.elf
