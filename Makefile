CC  = gcc
ASM = nasm
QEMU = qemu-system-x86_64
OVMF_CODE = /usr/share/OVMF/OVMF_CODE.fd
DOCKER_IMAGE = icda-toolchain
DOCKER_RUN = docker run --rm -v "$(CURDIR):/workspace" -w /workspace $(DOCKER_IMAGE)
SGDISK ?= /usr/sbin/sgdisk
SHELL_AUTOTEST ?= 0
SERIAL_SHELL_MIRROR ?= 0

CFLAGS = -ffreestanding -O0 -Wall -Wextra -fno-exceptions -fno-pie -no-pie \
         -fno-asynchronous-unwind-tables -Ikernel -fno-stack-protector \
         -mno-mmx -mno-sse -mno-sse2 \
         -DSERIAL_SHELL_MIRROR=$(SERIAL_SHELL_MIRROR)

all: kernel.iso kernel-usb.img

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

nvme.o: kernel/drivers/storage/nvme.c kernel/drivers/storage/nvme.h kernel/drivers/pci/pci.h \
        kernel/memory/pmm.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/drivers/storage/nvme.c -o nvme.o

ahci.o: kernel/drivers/storage/ahci.c kernel/drivers/storage/ahci.h kernel/drivers/pci/pci.h \
        kernel/memory/pmm.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/drivers/storage/ahci.c -o ahci.o

ata.o: kernel/drivers/storage/ata.c kernel/drivers/storage/ata.h
	$(CC) $(CFLAGS) -c kernel/drivers/storage/ata.c -o ata.o

block.o: kernel/drivers/storage/block.c kernel/drivers/storage/block.h
	$(CC) $(CFLAGS) -c kernel/drivers/storage/block.c -o block.o

partition.o: kernel/drivers/storage/partition.c kernel/drivers/storage/partition.h kernel/drivers/storage/block.h
	$(CC) $(CFLAGS) -c kernel/drivers/storage/partition.c -o partition.o

pci.o: kernel/drivers/pci/pci.c kernel/drivers/pci/pci.h kernel/firmware/acpi.h \
       kernel/cpu/lapic.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/drivers/pci/pci.c -o pci.o

initramfs.o: kernel/fs/initramfs.c kernel/fs/initramfs.h
	$(CC) $(CFLAGS) -c kernel/fs/initramfs.c -o initramfs.o

vfs.o: kernel/fs/vfs.c kernel/fs/vfs.h kernel/memory/heap.h
	$(CC) $(CFLAGS) -c kernel/fs/vfs.c -o vfs.o

persistfs.o: kernel/fs/persistfs.c kernel/fs/persistfs.h kernel/fs/vfs.h kernel/drivers/storage/ata.h
	$(CC) $(CFLAGS) -c kernel/fs/persistfs.c -o persistfs.o

fat32.o: kernel/fs/fat32.c kernel/fs/fat32.h kernel/fs/vfs.h kernel/drivers/storage/partition.h
	$(CC) $(CFLAGS) -c kernel/fs/fat32.c -o fat32.o

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

bootstage.o: kernel/diag/bootstage.c kernel/diag/bootstage.h \
             kernel/drivers/display/framebuffer.h kernel/drivers/display/vga.h \
             kernel/drivers/serial/serial.h
	$(CC) $(CFLAGS) -c kernel/diag/bootstage.c -o bootstage.o

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

userspace/hello.icx: userspace/hello.asm
	$(ASM) -f bin userspace/hello.asm -o userspace/hello.icx

userspace/pid.icx: userspace/pid.asm
	$(ASM) -f bin userspace/pid.asm -o userspace/pid.icx

userspace/ticker.icx: userspace/ticker.asm
	$(ASM) -f bin userspace/ticker.asm -o userspace/ticker.icx

userspace/hello_elf.o: userspace/hello_elf.asm
	$(ASM) -f elf64 userspace/hello_elf.asm -o userspace/hello_elf.o

userspace/pid_elf.o: userspace/pid_elf.asm
	$(ASM) -f elf64 userspace/pid_elf.asm -o userspace/pid_elf.o

userspace/hello.elf: userspace/hello_elf.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o userspace/hello.elf userspace/hello_elf.o

userspace/pid.elf: userspace/pid_elf.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o userspace/pid.elf userspace/pid_elf.o

shell_start.o: userspace/shell_start.asm
	$(ASM) -f elf64 userspace/shell_start.asm -o /tmp/icda-shell_start.o
	cp -f /tmp/icda-shell_start.o shell_start.o

shell.o: userspace/shell.c userspace/icda_sys.h
	$(CC) -ffreestanding -O0 -Wall -Wextra -fno-pie -no-pie -mcmodel=large -fno-asynchronous-unwind-tables -fno-stack-protector -mno-mmx -mno-sse -mno-sse2 -DSHELL_AUTOTEST=$(SHELL_AUTOTEST) -Iuserspace -c userspace/shell.c -o /tmp/icda-shell.o
	cp -f /tmp/icda-shell.o shell.o

userspace/shell.app: shell_start.o shell.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-shell.app shell_start.o shell.o
	cp -f /tmp/icda-shell.app userspace/shell.app

shell_blob.o: kernel/proc/shell_blob.asm userspace/shell.app
	$(ASM) -f elf64 kernel/proc/shell_blob.asm -o shell_blob.o

user_programs.o: kernel/proc/user_programs.asm userspace/hello.icx userspace/pid.icx userspace/ticker.icx userspace/hello.elf userspace/pid.elf
	$(ASM) -f elf64 kernel/proc/user_programs.asm -o user_programs.o

kernel.bin: kernel.o device.o vga.o framebuffer.o keyboard.o input.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs.o vfs.o persistfs.o fat32.o tty.o syscall.o console.o serial.o gdt.o idt.o isr.o pic.o lapic.o ioapic.o irq_controller.o acpi.o pmm.o heap.o vmm.o pf.o bootstage.o \
            sched.o sched_asm.o user.o user_enter.o user_demo_blob.o user_programs.o shell_blob.o boot.o gdt_flush.o isr_asm.o
	$(CC) -T kernel/linker.ld -o kernel.bin -ffreestanding -O0 -nostdlib \
	      -fno-pie -no-pie boot.o kernel.o device.o vga.o framebuffer.o keyboard.o input.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs.o vfs.o persistfs.o fat32.o tty.o syscall.o console.o serial.o \
	      gdt.o idt.o isr.o pic.o lapic.o ioapic.o irq_controller.o acpi.o pmm.o heap.o vmm.o pf.o \
	      bootstage.o sched.o sched_asm.o user.o user_enter.o user_demo_blob.o user_programs.o shell_blob.o gdt_flush.o isr_asm.o -lgcc

kernel.iso: kernel.bin
	mkdir -p isodir/boot/grub
	mkdir -p isodir/EFI/BOOT
	cp kernel.bin isodir/boot/kernel.bin
	cp boot/grub/grub.cfg isodir/boot/grub/grub.cfg
	grub-mkstandalone -O x86_64-efi -o isodir/EFI/BOOT/BOOTX64.EFI "boot/grub/grub.cfg=boot/grub/grub.cfg"
	grub-mkrescue -o kernel.iso isodir

kernel-usb.img: kernel.bin
	mkdir -p usbroot/EFI/BOOT
	mkdir -p usbroot/boot/grub
	cp kernel.bin usbroot/boot/kernel.bin
	cp boot/grub/grub-usb.cfg usbroot/boot/grub/grub.cfg
	grub-mkstandalone -O x86_64-efi -o usbroot/EFI/BOOT/BOOTX64.EFI "boot/grub/grub.cfg=boot/grub/grub-usb.cfg"
	rm -f kernel-usb.img
	dd if=/dev/zero of=kernel-usb.img bs=1M count=128
	$(SGDISK) -og kernel-usb.img
	$(SGDISK) -n 1:2048:0 -t 1:ef00 -c 1:"ICDA EFI" kernel-usb.img
	mformat -i kernel-usb.img@@1048576 -F ::
	mmd -i kernel-usb.img@@1048576 ::/EFI
	mmd -i kernel-usb.img@@1048576 ::/EFI/BOOT
	mmd -i kernel-usb.img@@1048576 ::/boot
	mmd -i kernel-usb.img@@1048576 ::/boot/grub
	mcopy -i kernel-usb.img@@1048576 usbroot/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i kernel-usb.img@@1048576 usbroot/boot/grub/grub.cfg ::/boot/grub/grub.cfg
	mcopy -i kernel-usb.img@@1048576 usbroot/boot/kernel.bin ::/boot/kernel.bin
clean:
ifeq ($(OS),Windows_NT)
	powershell -NoProfile -ExecutionPolicy Bypass -Command "Remove-Item -Force -ErrorAction SilentlyContinue *.o, kernel.bin, kernel.iso, kernel-usb.img, qemu-smoke.log; Remove-Item -Recurse -Force -ErrorAction SilentlyContinue isodir, usbroot"
else
	rm -f *.o kernel.bin kernel.iso kernel-usb.img qemu-smoke.log
	rm -rf isodir usbroot
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
