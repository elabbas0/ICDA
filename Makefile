CC  = gcc
ASM = nasm
QEMU = qemu-system-x86_64
OVMF_CODE = /usr/share/OVMF/OVMF_CODE.fd
DOCKER_IMAGE = icda-toolchain
DOCKER_RUN = docker run --rm -v "$(CURDIR):/workspace" -w /workspace $(DOCKER_IMAGE)

CFLAGS = -ffreestanding -O0 -Wall -Wextra -fno-exceptions -fno-pie -no-pie \
         -fno-asynchronous-unwind-tables -Ikernel -fno-stack-protector

all: kernel.iso

kernel.o: kernel/kernel.c
	$(CC) $(CFLAGS) -c kernel/kernel.c -o kernel.o

device.o: kernel/drivers/device.c kernel/drivers/device.h
	$(CC) $(CFLAGS) -c kernel/drivers/device.c -o device.o

vga.o: kernel/drivers/display/vga.c
	$(CC) $(CFLAGS) -c kernel/drivers/display/vga.c -o vga.o

framebuffer.o: kernel/drivers/display/framebuffer.c
	$(CC) $(CFLAGS) -c kernel/drivers/display/framebuffer.c -o framebuffer.o

keyboard.o: kernel/drivers/input/keyboard.c kernel/drivers/input/keyboard.h \
            kernel/cpu/isr.h kernel/cpu/pic.h
	$(CC) $(CFLAGS) -c kernel/drivers/input/keyboard.c -o keyboard.o

input.o: kernel/drivers/input/input.c kernel/drivers/input/input.h kernel/drivers/device.h
	$(CC) $(CFLAGS) -c kernel/drivers/input/input.c -o input.o

pci.o: kernel/drivers/pci/pci.c kernel/drivers/pci/pci.h kernel/firmware/acpi.h \
       kernel/cpu/lapic.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/drivers/pci/pci.c -o pci.o

initramfs.o: kernel/fs/initramfs.c kernel/fs/initramfs.h
	$(CC) $(CFLAGS) -c kernel/fs/initramfs.c -o initramfs.o

vfs.o: kernel/fs/vfs.c kernel/fs/vfs.h kernel/memory/heap.h
	$(CC) $(CFLAGS) -c kernel/fs/vfs.c -o vfs.o

tty.o: kernel/tty/tty.c kernel/tty/tty.h kernel/drivers/console/console.h \
       kernel/drivers/input/input.h kernel/memory/heap.h kernel/memory/pmm.h kernel/syscall/syscall.h
	$(CC) $(CFLAGS) -c kernel/tty/tty.c -o tty.o

syscall.o: kernel/syscall/syscall.c kernel/syscall/syscall.h kernel/fs/vfs.h kernel/proc/sched.h
	$(CC) $(CFLAGS) -c kernel/syscall/syscall.c -o syscall.o

console.o: kernel/drivers/console/console.c kernel/drivers/console/console.h \
           kernel/drivers/display/framebuffer.h kernel/drivers/display/vga.h \
           kernel/drivers/serial/serial.h
	$(CC) $(CFLAGS) -c kernel/drivers/console/console.c -o console.o

serial.o: kernel/drivers/serial/serial.c kernel/drivers/serial/serial.h
	$(CC) $(CFLAGS) -c kernel/drivers/serial/serial.c -o serial.o

gdt.o: kernel/cpu/gdt.c
	$(CC) $(CFLAGS) -c kernel/cpu/gdt.c -o gdt.o

idt.o: kernel/cpu/idt.c
	$(CC) $(CFLAGS) -c kernel/cpu/idt.c -o idt.o

isr.o: kernel/cpu/isr.c
	$(CC) $(CFLAGS) -c kernel/cpu/isr.c -o isr.o

pic.o: kernel/cpu/pic.c
	$(CC) $(CFLAGS) -c kernel/cpu/pic.c -o pic.o

lapic.o: kernel/cpu/lapic.c kernel/cpu/lapic.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/cpu/lapic.c -o lapic.o

ioapic.o: kernel/cpu/ioapic.c kernel/cpu/ioapic.h kernel/firmware/acpi.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/cpu/ioapic.c -o ioapic.o

irq_controller.o: kernel/cpu/irq_controller.c kernel/cpu/irq_controller.h kernel/cpu/pic.h
	$(CC) $(CFLAGS) -c kernel/cpu/irq_controller.c -o irq_controller.o

acpi.o: kernel/firmware/acpi.c kernel/firmware/acpi.h kernel/cpu/multiboot2.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/firmware/acpi.c -o acpi.o

boot.o: kernel/boot.asm
	$(ASM) -f elf64 kernel/boot.asm -o boot.o

gdt_flush.o: kernel/cpu/gdt_flush.asm
	$(ASM) -f elf64 kernel/cpu/gdt_flush.asm -o gdt_flush.o

isr_asm.o: kernel/cpu/isr.asm
	$(ASM) -f elf64 kernel/cpu/isr.asm -o isr_asm.o

pmm.o: kernel/memory/pmm.c kernel/memory/pmm.h kernel/cpu/multiboot2.h
	$(CC) $(CFLAGS) -c kernel/memory/pmm.c -o pmm.o

heap.o: kernel/memory/heap.c kernel/memory/heap.h kernel/memory/pmm.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/memory/heap.c -o heap.o

vmm.o: kernel/memory/vmm.c kernel/memory/vmm.h kernel/memory/pmm.h \
       kernel/cpu/multiboot2.h kernel/drivers/display/framebuffer.h
	$(CC) $(CFLAGS) -c kernel/memory/vmm.c -o vmm.o

pf.o: kernel/memory/pf.c kernel/memory/pf.h kernel/memory/vmm.h \
      kernel/memory/pmm.h kernel/cpu/isr.h kernel/drivers/display/framebuffer.h
	$(CC) $(CFLAGS) -c kernel/memory/pf.c -o pf.o

sched.o: kernel/proc/sched.c kernel/proc/sched.h kernel/proc/process.h \
         kernel/memory/pmm.h kernel/memory/vmm.h kernel/memory/pf.h \
         kernel/cpu/gdt.h kernel/drivers/display/framebuffer.h
	$(CC) $(CFLAGS) -c kernel/proc/sched.c -o sched.o

sched_asm.o: kernel/proc/sched.asm
	$(ASM) -f elf64 kernel/proc/sched.asm -o sched_asm.o

user.o: kernel/proc/user.c kernel/proc/user.h kernel/proc/process.h kernel/memory/vmm.h kernel/memory/pf.h
	$(CC) $(CFLAGS) -c kernel/proc/user.c -o user.o

user_enter.o: kernel/proc/user_enter.asm
	$(ASM) -f elf64 kernel/proc/user_enter.asm -o user_enter.o

user_demo_blob.o: kernel/proc/user_demo.asm
	$(ASM) -f elf64 kernel/proc/user_demo.asm -o user_demo_blob.o

kernel.bin: kernel.o device.o vga.o framebuffer.o keyboard.o input.o pci.o initramfs.o vfs.o tty.o syscall.o console.o serial.o gdt.o idt.o isr.o pic.o lapic.o ioapic.o irq_controller.o acpi.o pmm.o heap.o vmm.o pf.o \
            sched.o sched_asm.o user.o user_enter.o user_demo_blob.o boot.o gdt_flush.o isr_asm.o
	$(CC) -T kernel/linker.ld -o kernel.bin -ffreestanding -O0 -nostdlib \
	      -fno-pie -no-pie boot.o kernel.o device.o vga.o framebuffer.o keyboard.o input.o pci.o initramfs.o vfs.o tty.o syscall.o console.o serial.o \
	      gdt.o idt.o isr.o pic.o lapic.o ioapic.o irq_controller.o acpi.o pmm.o heap.o vmm.o pf.o \
	      sched.o sched_asm.o user.o user_enter.o user_demo_blob.o gdt_flush.o isr_asm.o -lgcc

kernel.iso: kernel.bin
	mkdir -p isodir/boot/grub
	cp kernel.bin isodir/boot/kernel.bin
	cp boot/grub/grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o kernel.iso isodir
clean:
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-Item -Force -ErrorAction SilentlyContinue *.o, kernel.bin, kernel.iso, qemu-smoke.log; Remove-Item -Recurse -Force -ErrorAction SilentlyContinue isodir"
else
	rm -f *.o kernel.bin kernel.iso qemu-smoke.log
	rm -rf isodir
endif

qemu-headless: kernel.iso
	$(QEMU) -cdrom kernel.iso -m 256M -serial stdio -display none -monitor none -no-reboot

qemu: kernel.iso
	$(QEMU) -cdrom kernel.iso -m 256M -serial stdio -no-reboot

qemu-uefi-headless: kernel.iso
	$(QEMU) -bios $(OVMF_CODE) -cdrom kernel.iso -m 256M -serial stdio -display none -monitor none -no-reboot

qemu-uefi: kernel.iso
	$(QEMU) -bios $(OVMF_CODE) -cdrom kernel.iso -m 256M -serial stdio -no-reboot

qemu-smoke: kernel.iso
	sh scripts/qemu-smoke.sh kernel.iso

docker-image:
	docker build -t $(DOCKER_IMAGE) .

docker-build: docker-image
	$(DOCKER_RUN) make

docker-qemu: docker-image
	$(DOCKER_RUN) make qemu

docker-qemu-headless: docker-image
	$(DOCKER_RUN) make qemu-headless

docker-qemu-uefi: docker-image
	$(DOCKER_RUN) make qemu-uefi

docker-qemu-uefi-headless: docker-image
	$(DOCKER_RUN) make qemu-uefi-headless

docker-smoke: docker-image
	$(DOCKER_RUN) make qemu-smoke

.PHONY: all clean qemu qemu-headless qemu-uefi qemu-uefi-headless qemu-smoke docker-image docker-build docker-qemu docker-qemu-headless docker-qemu-uefi docker-qemu-uefi-headless docker-smoke
