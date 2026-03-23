CC  = gcc
ASM = nasm

CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-pie -no-pie \
         -fno-asynchronous-unwind-tables -Ikernel

all: kernel.iso

kernel.o: kernel/kernel.c
	$(CC) $(CFLAGS) -c kernel/kernel.c -o kernel.o

vga.o: kernel/drivers/display/vga.c
	$(CC) $(CFLAGS) -c kernel/drivers/display/vga.c -o vga.o

framebuffer.o: kernel/drivers/display/framebuffer.c
	$(CC) $(CFLAGS) -c kernel/drivers/display/framebuffer.c -o framebuffer.o

gdt.o: kernel/gdt.c
	$(CC) $(CFLAGS) -c kernel/gdt.c -o gdt.o

idt.o: kernel/idt.c
	$(CC) $(CFLAGS) -c kernel/idt.c -o idt.o

isr.o: kernel/isr.c
	$(CC) $(CFLAGS) -c kernel/isr.c -o isr.o

pic.o: kernel/pic.c
	$(CC) $(CFLAGS) -c kernel/pic.c -o pic.o

boot.o: kernel/kernel.asm
	$(ASM) -f elf64 kernel/kernel.asm -o boot.o

gdt_flush.o: kernel/gdt_flush.asm
	$(ASM) -f elf64 kernel/gdt_flush.asm -o gdt_flush.o

isr_asm.o: kernel/isr.asm
	$(ASM) -f elf64 kernel/isr.asm -o isr_asm.o

kernel.bin: kernel.o vga.o framebuffer.o gdt.o idt.o isr.o pic.o \
            boot.o gdt_flush.o isr_asm.o
	$(CC) -T kernel/linker.ld -o kernel.bin -ffreestanding -O2 -nostdlib \
	      -fno-pie -no-pie boot.o kernel.o vga.o framebuffer.o \
	      gdt.o idt.o isr.o pic.o gdt_flush.o isr_asm.o -lgcc

kernel.iso: kernel.bin
	mkdir -p isodir/boot/grub
	cp kernel.bin isodir/boot/kernel.bin
	cp boot/grub/grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o kernel.iso isodir

clean:
	rm -f *.o kernel.bin kernel.iso
	rm -rf isodir