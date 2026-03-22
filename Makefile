CC = gcc
ASM = nasm

CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-pie -no-pie -fno-asynchronous-unwind-tables -Ikernel

all: kernel.iso

# compile C
kernel.o: kernel/kernel.c
	$(CC) $(CFLAGS) -c kernel/kernel.c -o kernel.o

# compile VGA driver
vga.o: kernel/drivers/display/vga.c
	$(CC) $(CFLAGS) -c kernel/drivers/display/vga.c -o vga.o

# compile ASM
boot.o: kernel/kernel.asm
	$(ASM) -f elf64 kernel/kernel.asm -o boot.o

# link everything together
kernel.bin: kernel.o vga.o boot.o
	$(CC) -T kernel/linker.ld -o kernel.bin -ffreestanding -O2 -nostdlib -fno-pie -no-pie boot.o kernel.o vga.o -lgcc

# package into bootable ISO
kernel.iso: kernel.bin
	mkdir -p isodir/boot/grub
	cp kernel.bin isodir/boot/kernel.bin
	cp boot/grub/grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o kernel.iso isodir

clean:
	rm -f *.o kernel.bin kernel.iso
	rm -rf isodir