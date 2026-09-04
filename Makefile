CC  = gcc
ASM = nasm
QEMU = qemu-system-x86_64
OVMF_CODE = /usr/share/OVMF/OVMF_CODE.fd
DOCKER_IMAGE = icda-toolchain
DOCKER_RUN = docker run --rm -v "$(CURDIR):/workspace" -w /workspace $(DOCKER_IMAGE)
SGDISK ?= /usr/sbin/sgdisk
SHELL_AUTOTEST ?= 0
SERIAL_SHELL_MIRROR ?= 0
AUDIO_WAVS := $(wildcard userspace/*.wav)
ICON_ICOS := $(wildcard resources/icons/*.ico)

CFLAGS = -ffreestanding -O0 -Wall -Wextra -fno-exceptions -fno-pie -no-pie \
         -fno-asynchronous-unwind-tables -Ikernel -fno-stack-protector \
         -mno-mmx -mno-sse -mno-sse2 \
         -DSERIAL_SHELL_MIRROR=$(SERIAL_SHELL_MIRROR)

# Userspace (the whole GUI stack - WM compositing, libicda drawing, apps)
# runs optimized: at -O0 the 1920x1080 compositing math made real hardware
# crawl, which read as "1 fps".  Kernel stays -O0 (boot path is short).
USR_CFLAGS = -ffreestanding -O2 -Wall -Wextra -fno-pie -no-pie -mcmodel=large \
             -fno-asynchronous-unwind-tables -fno-stack-protector \
             -mno-mmx -mno-sse -mno-sse2 -Iuserspace

all: kernel.iso kernel-usb.img

kernel.o: kernel/kernel.c Makefile
	$(CC) $(CFLAGS) -c kernel/kernel.c -o kernel.o

device.o: kernel/drivers/device.c kernel/drivers/device.h
	$(CC) $(CFLAGS) -c kernel/drivers/device.c -o device.o

speaker.o: kernel/drivers/audio/speaker.c kernel/drivers/audio/speaker.h \
           kernel/proc/sched.h
	$(CC) $(CFLAGS) -c kernel/drivers/audio/speaker.c -o speaker.o

playback.o: kernel/drivers/audio/playback.c kernel/drivers/audio/playback.h Makefile \
            kernel/drivers/audio/hda.h kernel/drivers/console/console.h \
            kernel/fs/vfs.h kernel/memory/heap.h kernel/proc/sched.h
	$(CC) $(CFLAGS) -c kernel/drivers/audio/playback.c -o playback.o

hda.o: kernel/drivers/audio/hda.c kernel/drivers/audio/hda.h Makefile \
        kernel/drivers/pci/pci.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/drivers/audio/hda.c -o hda.o

e1000.o: kernel/drivers/net/e1000.c kernel/drivers/net/e1000.h Makefile \
         kernel/drivers/pci/pci.h kernel/memory/pmm.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/drivers/net/e1000.c -o e1000.o

virtio_net.o: kernel/drivers/net/virtio_net.c kernel/drivers/net/virtio_net.h Makefile \
             kernel/drivers/pci/pci.h kernel/memory/pmm.h kernel/memory/vmm.h kernel/proc/sched.h
	$(CC) $(CFLAGS) -c kernel/drivers/net/virtio_net.c -o virtio_net.o

net_drv.o: kernel/drivers/net/net_drv.c kernel/drivers/net/net_drv.h Makefile \
           kernel/drivers/net/e1000.h kernel/drivers/net/virtio_net.h kernel/drivers/serial/serial.h
	$(CC) $(CFLAGS) -c kernel/drivers/net/net_drv.c -o net_drv.o

net.o: kernel/net/net.c kernel/net/net.h kernel/net/tls.h Makefile \
       kernel/drivers/net/net_drv.h kernel/fs/vfs.h kernel/memory/heap.h kernel/proc/sched.h
	$(CC) $(CFLAGS) -c kernel/net/net.c -o net.o

sha256.o: kernel/crypto/sha256.c kernel/crypto/sha256.h
	$(CC) $(CFLAGS) -c kernel/crypto/sha256.c -o sha256.o

sha1.o: kernel/crypto/sha1.c kernel/crypto/sha1.h
	$(CC) $(CFLAGS) -c kernel/crypto/sha1.c -o sha1.o

aes.o: kernel/crypto/aes.c kernel/crypto/aes.h
	$(CC) $(CFLAGS) -c kernel/crypto/aes.c -o aes.o

bn.o: kernel/crypto/bn.c kernel/crypto/bn.h
	$(CC) $(CFLAGS) -c kernel/crypto/bn.c -o bn.o

rsa.o: kernel/crypto/rsa.c kernel/crypto/rsa.h kernel/crypto/bn.h
	$(CC) $(CFLAGS) -c kernel/crypto/rsa.c -o rsa.o

x25519.o: kernel/crypto/x25519.c kernel/crypto/x25519.h
	$(CC) $(CFLAGS) -c kernel/crypto/x25519.c -o x25519.o

gcm.o: kernel/crypto/gcm.c kernel/crypto/gcm.h kernel/crypto/aes.h
	$(CC) $(CFLAGS) -c kernel/crypto/gcm.c -o gcm.o

tls.o: kernel/net/tls.c kernel/net/tls.h kernel/crypto/sha256.h kernel/crypto/sha1.h kernel/crypto/hmac.h kernel/crypto/aes.h kernel/crypto/rsa.h kernel/net/net.h
	$(CC) $(CFLAGS) -c kernel/net/tls.c -o tls.o

sb16.o: kernel/drivers/audio/sb16.c kernel/drivers/audio/sb16.h \
         kernel/memory/pmm.h kernel/memory/vmm.h kernel/proc/sched.h
	$(CC) $(CFLAGS) -c kernel/drivers/audio/sb16.c -o sb16.o

vga.o: kernel/drivers/display/vga.c
	$(CC) $(CFLAGS) -c kernel/drivers/display/vga.c -o vga.o

framebuffer.o: kernel/drivers/display/framebuffer.c
	$(CC) $(CFLAGS) -c kernel/drivers/display/framebuffer.c -o framebuffer.o

gpu.o: kernel/drivers/display/gpu.c kernel/drivers/display/gpu.h kernel/drivers/display/framebuffer.h
	$(CC) $(CFLAGS) -c kernel/drivers/display/gpu.c -o gpu.o

power.o: kernel/power/power.c kernel/power/power.h kernel/firmware/acpi.h
	$(CC) $(CFLAGS) -c kernel/power/power.c -o power.o

keyboard.o: kernel/drivers/input/keyboard.c kernel/drivers/input/keyboard.h \
            kernel/cpu/isr.h kernel/cpu/pic.h
	$(CC) $(CFLAGS) -c kernel/drivers/input/keyboard.c -o keyboard.o

input.o: kernel/drivers/input/input.c kernel/drivers/input/input.h kernel/drivers/device.h
	$(CC) $(CFLAGS) -c kernel/drivers/input/input.c -o input.o

mouse.o: kernel/drivers/input/mouse.c kernel/drivers/input/mouse.h kernel/cpu/isr.h
	$(CC) $(CFLAGS) -c kernel/drivers/input/mouse.c -o mouse.o

shm.o: kernel/ipc/shm.c kernel/ipc/shm.h kernel/memory/pmm.h kernel/memory/vmm.h kernel/proc/sched.h
	$(CC) $(CFLAGS) -c kernel/ipc/shm.c -o shm.o

msgq.o: kernel/ipc/msgq.c kernel/ipc/msgq.h kernel/proc/sched.h
	$(CC) $(CFLAGS) -c kernel/ipc/msgq.c -o msgq.o

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

initramfs.o: kernel/fs/initramfs.c kernel/fs/initramfs.h kernel/fs/audio_assets_gen.h kernel/fs/audio_assets_gen.c kernel/fs/icon_assets_gen.h kernel/fs/icon_assets_gen.c Makefile
	$(CC) $(CFLAGS) -c kernel/fs/initramfs.c -o initramfs.o

initramfs_install.o: kernel/fs/initramfs.c kernel/fs/initramfs.h kernel/fs/audio_assets_gen.h kernel/fs/audio_assets_gen.c kernel/fs/icon_assets_gen.h kernel/fs/icon_assets_gen.c Makefile
	$(CC) $(CFLAGS) -DINITRAMFS_INCLUDE_AUDIO_ASSETS=0 -DINITRAMFS_INCLUDE_ICON_ASSETS=0 -c kernel/fs/initramfs.c -o initramfs_install.o

install.o: kernel/fs/install.c kernel/fs/install.h kernel/fs/initramfs.h kernel/fs/vfs.h \
           kernel/fs/boot_assets.h kernel/fs/persistfs.h kernel/drivers/storage/partition.h
	$(CC) $(CFLAGS) -c kernel/fs/install.c -o install.o

diskfmt.o: kernel/fs/diskfmt.c kernel/fs/diskfmt.h kernel/drivers/storage/block.h \
           kernel/drivers/storage/partition.h kernel/fs/fat32.h kernel/fs/exfat.h kernel/fs/ntfs.h
	$(CC) $(CFLAGS) -c kernel/fs/diskfmt.c -o diskfmt.o

audio_assets_gen.o: kernel/fs/audio_assets_gen.c kernel/fs/audio_assets_gen.h
	$(CC) $(CFLAGS) -c kernel/fs/audio_assets_gen.c -o audio_assets_gen.o

vfs.o: kernel/fs/vfs.c kernel/fs/vfs.h kernel/memory/heap.h
	$(CC) $(CFLAGS) -c kernel/fs/vfs.c -o vfs.o

fd.o: kernel/fs/fd.c kernel/fs/fd.h kernel/fs/vfs.h kernel/proc/process.h kernel/syscall/syscall.h
	$(CC) $(CFLAGS) -c kernel/fs/fd.c -o fd.o

persistfs.o: kernel/fs/persistfs.c kernel/fs/persistfs.h kernel/fs/vfs.h kernel/drivers/storage/ata.h
	$(CC) $(CFLAGS) -c kernel/fs/persistfs.c -o persistfs.o

fat32.o: kernel/fs/fat32.c kernel/fs/fat32.h kernel/fs/vfs.h kernel/drivers/storage/partition.h
	$(CC) $(CFLAGS) -c kernel/fs/fat32.c -o fat32.o

exfat.o: kernel/fs/exfat.c kernel/fs/exfat.h kernel/fs/vfs.h kernel/drivers/storage/partition.h
	$(CC) $(CFLAGS) -c kernel/fs/exfat.c -o exfat.o

ntfs.o: kernel/fs/ntfs.c kernel/fs/ntfs.h kernel/fs/vfs.h kernel/drivers/storage/partition.h
	$(CC) $(CFLAGS) -c kernel/fs/ntfs.c -o ntfs.o

tty.o: kernel/tty/tty.c kernel/tty/tty.h kernel/drivers/console/console.h \
       kernel/drivers/input/input.h kernel/memory/heap.h kernel/memory/pmm.h kernel/syscall/syscall.h
	$(CC) $(CFLAGS) -c kernel/tty/tty.c -o tty.o

vt.o: kernel/vt/vt.c kernel/vt/vt.h kernel/proc/sched.h kernel/drivers/console/console.h
	$(CC) $(CFLAGS) -c kernel/vt/vt.c -o vt.o

syscall.o: kernel/syscall/syscall.c kernel/syscall/syscall.h kernel/fs/vfs.h kernel/proc/sched.h kernel/fs/install.h kernel/fs/diskfmt.h kernel/net/net.h kernel/memory/pmm.h kernel/memory/vmm.h
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

splash.o: kernel/diag/splash.c kernel/diag/splash.h \
          kernel/drivers/display/framebuffer.h kernel/drivers/display/font.h
	$(CC) $(CFLAGS) -c kernel/diag/splash.c -o splash.o

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

user.o: kernel/proc/user.c kernel/proc/user.h kernel/proc/process.h kernel/proc/elf.h kernel/memory/vmm.h kernel/memory/pf.h
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

userspace/argc_elf.o: userspace/argc_elf.asm
	$(ASM) -f elf64 userspace/argc_elf.asm -o userspace/argc_elf.o

userspace/hello.elf: userspace/hello_elf.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o userspace/hello.elf userspace/hello_elf.o

userspace/pid.elf: userspace/pid_elf.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o userspace/pid.elf userspace/pid_elf.o

userspace/argc.elf: userspace/argc_elf.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o userspace/argc.elf userspace/argc_elf.o

nptestlx_start.o: userspace/nptestlx_start.asm
	$(ASM) -f elf64 userspace/nptestlx_start.asm -o /tmp/icda-nptestlx_start.o
	cp -f /tmp/icda-nptestlx_start.o nptestlx_start.o

nptestlx.o: userspace/nptestlx.c userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/nptestlx.c -o /tmp/icda-nptestlx.o
	cp -f /tmp/icda-nptestlx.o nptestlx.o

userspace/nptestlx.elf: nptestlx_start.o nptestlx.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o userspace/nptestlx.elf nptestlx_start.o nptestlx.o

shell_start.o: userspace/shell_start.asm
	$(ASM) -f elf64 userspace/shell_start.asm -o /tmp/icda-shell_start.o
	cp -f /tmp/icda-shell_start.o shell_start.o

editor_start.o: userspace/editor_start.asm
	$(ASM) -f elf64 userspace/editor_start.asm -o /tmp/icda-editor_start.o
	cp -f /tmp/icda-editor_start.o editor_start.o

shell.o: userspace/shell.c userspace/icda_sys.h Makefile
	$(CC) $(USR_CFLAGS) -DSHELL_AUTOTEST=$(SHELL_AUTOTEST) -Iuserspace -c userspace/shell.c -o /tmp/icda-shell.o
	cp -f /tmp/icda-shell.o shell.o

audioplay.o: userspace/audioplay.c userspace/gui.h userspace/gui_proto.h userspace/libicda.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/audioplay.c -o /tmp/icda-audioplay.o
	cp -f /tmp/icda-audioplay.o audioplay.o

editor.o: userspace/editor.c userspace/gui.h userspace/gui_proto.h userspace/libicda.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/editor.c -o /tmp/icda-editor.o
	cp -f /tmp/icda-editor.o editor.o

diskman.o: userspace/diskman.c userspace/gui.h userspace/gui_proto.h userspace/libicda.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/diskman.c -o /tmp/icda-diskman.o
	cp -f /tmp/icda-diskman.o diskman.o

userspace/shell.app: shell_start.o shell.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-shell.app shell_start.o shell.o
	cp -f /tmp/icda-shell.app userspace/shell.app

userspace/audioplay.app: crt0.o audioplay.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-audioplay.app crt0.o audioplay.o gui.o libicda.o
	cp -f /tmp/icda-audioplay.app userspace/audioplay.app

userspace/editor.app: crt0.o editor.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-editor.app crt0.o editor.o gui.o libicda.o
	cp -f /tmp/icda-editor.app userspace/editor.app

userspace/diskman.app: crt0.o diskman.o gui.o libicda.o userspace/user.ld

userspace/taskman.app: crt0.o taskman.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-taskman.app crt0.o taskman.o gui.o libicda.o
	cp -f /tmp/icda-taskman.app userspace/taskman.app

taskman.o: userspace/taskman.c userspace/gui.h userspace/gui_proto.h userspace/libicda.h userspace/font.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/taskman.c -o /tmp/icda-taskman.o
	cp -f /tmp/icda-taskman.o taskman.o
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-diskman.app crt0.o diskman.o gui.o libicda.o
	cp -f /tmp/icda-diskman.app userspace/diskman.app

browser_start.o: userspace/browser_start.asm
	$(ASM) -f elf64 userspace/browser_start.asm -o /tmp/icda-browser_start.o
	cp -f /tmp/icda-browser_start.o browser_start.o

browser.o: userspace/browser.c userspace/gui.h userspace/gui_proto.h userspace/libicda.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/browser.c -o /tmp/icda-browser.o
	cp -f /tmp/icda-browser.o browser.o

userspace/browser.app: crt0.o browser_start.o browser.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-browser.app browser_start.o browser.o gui.o libicda.o
	cp -f /tmp/icda-browser.app userspace/browser.app

shell_blob.o: kernel/proc/shell_blob.asm userspace/shell.app
	$(ASM) -f elf64 kernel/proc/shell_blob.asm -o shell_blob.o

kernel/fs/bootx64-install.efi: boot/grub/grub-install.cfg
	grub-mkstandalone -O x86_64-efi -o kernel/fs/bootx64-install.efi "boot/grub/grub.cfg=boot/grub/grub-install.cfg"

boot_assets.o: kernel/proc/boot_assets.asm kernel/fs/bootx64-install.efi kernel/install-kernel.bin
	$(ASM) -f elf64 kernel/proc/boot_assets.asm -o boot_assets.o

kernel/fs/audio_assets_gen.c kernel/proc/audio_assets.asm &: Makefile $(AUDIO_WAVS)
	@mkdir -p kernel/fs kernel/proc
	@printf '#include <stdint.h>\n#include "audio_assets_gen.h"\n\n' > kernel/fs/audio_assets_gen.c
	@rm -f kernel/fs/.audio_assets.externs.tmp kernel/fs/.audio_assets.entries.tmp
	@touch kernel/fs/.audio_assets.externs.tmp kernel/fs/.audio_assets.entries.tmp
	@printf 'bits 64\n\nsection .rodata\n' > kernel/proc/audio_assets.asm
	@count=0; \
	for f in userspace/*.wav; do \
		if [ ! -f "$$f" ]; then continue; fi; \
		base=$$(basename "$$f"); \
		sym=$$(printf '%s' "$$base" | sed 's/[^A-Za-z0-9]/_/g'); \
		printf 'extern const char asset_%s_start[];\n' "$$sym" >> kernel/fs/.audio_assets.externs.tmp; \
		printf 'extern const char asset_%s_end[];\n' "$$sym" >> kernel/fs/.audio_assets.externs.tmp; \
		printf 'global asset_%s_start\n' "$$sym" >> kernel/proc/audio_assets.asm; \
		printf 'global asset_%s_end\n' "$$sym" >> kernel/proc/audio_assets.asm; \
		printf 'asset_%s_start:\n    incbin "%s"\nasset_%s_end:\n\n' "$$sym" "$$f" "$$sym" >> kernel/proc/audio_assets.asm; \
		printf '    { "/usr/share/audio/%s", asset_%s_start, asset_%s_end },\n' "$$base" "$$sym" "$$sym" >> kernel/fs/.audio_assets.entries.tmp; \
		count=$$((count + 1)); \
	done; \
	cat kernel/fs/.audio_assets.externs.tmp >> kernel/fs/audio_assets_gen.c; \
	printf '\nconst generated_audio_asset_t generated_audio_assets[] = {\n' >> kernel/fs/audio_assets_gen.c; \
	if [ "$$count" -eq 0 ]; then \
		printf '    { 0, 0, 0 }\n};\n' >> kernel/fs/audio_assets_gen.c; \
		printf 'const uint64_t generated_audio_asset_count = 0;\n' >> kernel/fs/audio_assets_gen.c; \
	else \
		cat kernel/fs/.audio_assets.entries.tmp >> kernel/fs/audio_assets_gen.c; \
		printf '};\nconst uint64_t generated_audio_asset_count = %s;\n' "$$count" >> kernel/fs/audio_assets_gen.c; \
	fi; \
	rm -f kernel/fs/.audio_assets.externs.tmp kernel/fs/.audio_assets.entries.tmp; \
	printf '\nsection .note.GNU-stack noalloc noexec nowrite progbits\n' >> kernel/proc/audio_assets.asm

audio_assets.o: kernel/proc/audio_assets.asm kernel/fs/audio_assets_gen.c
	$(ASM) -f elf64 kernel/proc/audio_assets.asm -o audio_assets.o

kernel/fs/icon_assets_gen.c kernel/proc/icon_assets.asm &: Makefile $(ICON_ICOS)
	@mkdir -p kernel/fs kernel/proc resources/icons
	@printf '#include <stdint.h>\n#include "icon_assets_gen.h"\n\n' > kernel/fs/icon_assets_gen.c
	@rm -f kernel/fs/.icon_assets.externs.tmp kernel/fs/.icon_assets.entries.tmp
	@touch kernel/fs/.icon_assets.externs.tmp kernel/fs/.icon_assets.entries.tmp
	@printf 'bits 64\n\nsection .rodata\n' > kernel/proc/icon_assets.asm
	@count=0; \
	for f in resources/icons/*.ico; do \
		if [ ! -f "$$f" ]; then continue; fi; \
		base=$$(basename "$$f"); \
		sym=$$(printf '%s' "$$base" | sed 's/[^A-Za-z0-9]/_/g'); \
		printf 'extern const char icon_asset_%s_start[];\n' "$$sym" >> kernel/fs/.icon_assets.externs.tmp; \
		printf 'extern const char icon_asset_%s_end[];\n' "$$sym" >> kernel/fs/.icon_assets.externs.tmp; \
		printf 'global icon_asset_%s_start\n' "$$sym" >> kernel/proc/icon_assets.asm; \
		printf 'global icon_asset_%s_end\n' "$$sym" >> kernel/proc/icon_assets.asm; \
		printf 'icon_asset_%s_start:\n    incbin "%s"\nicon_asset_%s_end:\n\n' "$$sym" "$$f" "$$sym" >> kernel/proc/icon_assets.asm; \
		printf '    { "/usr/share/icons/%s", icon_asset_%s_start, icon_asset_%s_end },\n' "$$base" "$$sym" "$$sym" >> kernel/fs/.icon_assets.entries.tmp; \
		count=$$((count + 1)); \
	done; \
	cat kernel/fs/.icon_assets.externs.tmp >> kernel/fs/icon_assets_gen.c; \
	printf '\nconst generated_icon_asset_t generated_icon_assets[] = {\n' >> kernel/fs/icon_assets_gen.c; \
	if [ "$$count" -eq 0 ]; then \
		printf '    { 0, 0, 0 }\n};\n' >> kernel/fs/icon_assets_gen.c; \
		printf 'const uint64_t generated_icon_asset_count = 0;\n' >> kernel/fs/icon_assets_gen.c; \
	else \
		cat kernel/fs/.icon_assets.entries.tmp >> kernel/fs/icon_assets_gen.c; \
		printf '};\nconst uint64_t generated_icon_asset_count = %s;\n' "$$count" >> kernel/fs/icon_assets_gen.c; \
	fi; \
	rm -f kernel/fs/.icon_assets.externs.tmp kernel/fs/.icon_assets.entries.tmp; \
	printf '\nsection .note.GNU-stack noalloc noexec nowrite progbits\n' >> kernel/proc/icon_assets.asm

icon_assets_gen.o: kernel/fs/icon_assets_gen.c kernel/fs/icon_assets_gen.h
	$(CC) $(CFLAGS) -c kernel/fs/icon_assets_gen.c -o icon_assets_gen.o

icon_assets.o: kernel/proc/icon_assets.asm kernel/fs/icon_assets_gen.c
	$(ASM) -f elf64 kernel/proc/icon_assets.asm -o icon_assets.o

curl_start.o: userspace/curl_start.asm
	$(ASM) -f elf64 userspace/curl_start.asm -o /tmp/icda-curl_start.o
	cp -f /tmp/icda-curl_start.o curl_start.o

curl.o: userspace/curl.c userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/curl.c -o /tmp/icda-curl.o
	cp -f /tmp/icda-curl.o curl.o

userspace/curl.app: curl_start.o curl.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-curl.app curl_start.o curl.o
	cp -f /tmp/icda-curl.app userspace/curl.app

nptest_start.o: userspace/nptest_start.asm
	$(ASM) -f elf64 userspace/nptest_start.asm -o /tmp/icda-nptest_start.o
	cp -f /tmp/icda-nptest_start.o nptest_start.o

nptest.o: userspace/nptest.c userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/nptest.c -o /tmp/icda-nptest.o
	cp -f /tmp/icda-nptest.o nptest.o

userspace/nptest.app: nptest_start.o nptest.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-nptest.app nptest_start.o nptest.o
	cp -f /tmp/icda-nptest.app userspace/nptest.app

gui.o: userspace/gui.c userspace/gui.h userspace/gui_proto.h userspace/font.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/gui.c -o /tmp/icda-gui.o
	cp -f /tmp/icda-gui.o gui.o

crt0.o: userspace/crt0.asm
	$(ASM) -f elf64 userspace/crt0.asm -o /tmp/icda-crt0.o
	cp -f /tmp/icda-crt0.o crt0.o

libicda.o: userspace/libicda.c userspace/libicda.h userspace/icon_data.h userspace/font.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/libicda.c -o /tmp/icda-libicda.o
	cp -f /tmp/icda-libicda.o libicda.o

gui_demo.o: userspace/gui_demo.c userspace/libicda.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/gui_demo.c -o /tmp/icda-gui_demo.o
	cp -f /tmp/icda-gui_demo.o gui_demo.o

userspace/gui_demo.app: gui_demo.o crt0.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-gui_demo.app gui_demo.o crt0.o gui.o libicda.o
	cp -f /tmp/icda-gui_demo.app userspace/gui_demo.app

wm.o: userspace/wm.c userspace/gui_proto.h userspace/libicda.h userspace/icon_data.h userspace/font.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/wm.c -o /tmp/icda-wm.o
	cp -f /tmp/icda-wm.o wm.o

userspace/wm.app: crt0.o wm.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-wm.app crt0.o wm.o gui.o libicda.o
	cp -f /tmp/icda-wm.app userspace/wm.app

desktop.o: userspace/desktop.c userspace/gui.h userspace/gui_proto.h userspace/libicda.h userspace/font.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/desktop.c -o /tmp/icda-desktop.o
	cp -f /tmp/icda-desktop.o desktop.o

userspace/desktop.app: crt0.o desktop.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-desktop.app crt0.o desktop.o gui.o libicda.o
	cp -f /tmp/icda-desktop.app userspace/desktop.app

terminal.o: userspace/terminal.c userspace/gui.h userspace/libicda.h userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/terminal.c -o /tmp/icda-terminal.o
	cp -f /tmp/icda-terminal.o terminal.o

userspace/terminal.app: crt0.o terminal.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-terminal.app crt0.o terminal.o gui.o libicda.o
	cp -f /tmp/icda-terminal.app userspace/terminal.app

user_programs.o: kernel/proc/user_programs.asm userspace/hello.icx userspace/pid.icx userspace/ticker.icx userspace/hello.elf userspace/pid.elf userspace/argc.elf userspace/audioplay.app userspace/editor.app userspace/diskman.app userspace/curl.app userspace/wm.app userspace/desktop.app userspace/terminal.app userspace/gui_demo.app userspace/taskman.app userspace/browser.app userspace/nptest.app userspace/nptestlx.elf
	$(ASM) -f elf64 kernel/proc/user_programs.asm -o user_programs.o

kernel/install-kernel.bin: kernel.o device.o speaker.o playback.o hda.o e1000.o virtio_net.o net_drv.o net.o vga.o framebuffer.o gpu.o keyboard.o input.o mouse.o shm.o msgq.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs_install.o install.o diskfmt.o vfs.o fd.o persistfs.o fat32.o exfat.o ntfs.o tty.o syscall.o console.o serial.o gdt.o idt.o isr.o pic.o lapic.o ioapic.o irq_controller.o acpi.o pmm.o heap.o vmm.o pf.o bootstage.o splash.o power.o vt.o \
            sched.o sched_asm.o user.o user_enter.o user_demo_blob.o user_programs.o shell_blob.o boot.o gdt_flush.o isr_asm.o \
            sha256.o sha1.o aes.o bn.o rsa.o x25519.o gcm.o tls.o
	$(CC) -T kernel/linker.ld -o kernel/install-kernel.bin -ffreestanding -O0 -nostdlib \
	      -fno-pie -no-pie boot.o kernel.o device.o speaker.o playback.o hda.o e1000.o virtio_net.o net_drv.o net.o vga.o framebuffer.o gpu.o keyboard.o input.o mouse.o shm.o msgq.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs_install.o install.o diskfmt.o vfs.o fd.o persistfs.o fat32.o exfat.o ntfs.o tty.o syscall.o console.o serial.o power.o vt.o \
	      gdt.o idt.o isr.o pic.o lapic.o ioapic.o irq_controller.o acpi.o pmm.o heap.o vmm.o pf.o \
	      bootstage.o splash.o sched.o sched_asm.o user.o user_enter.o user_demo_blob.o user_programs.o shell_blob.o gdt_flush.o isr_asm.o \
	      sha256.o sha1.o aes.o bn.o rsa.o x25519.o gcm.o tls.o -lgcc

kernel.bin: kernel.o device.o speaker.o playback.o hda.o e1000.o virtio_net.o net_drv.o net.o vga.o framebuffer.o gpu.o keyboard.o input.o mouse.o shm.o msgq.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs.o install.o diskfmt.o audio_assets_gen.o icon_assets_gen.o icon_assets.o vfs.o fd.o persistfs.o fat32.o exfat.o ntfs.o tty.o syscall.o console.o serial.o gdt.o idt.o isr.o pic.o lapic.o ioapic.o irq_controller.o acpi.o pmm.o heap.o vmm.o pf.o bootstage.o splash.o power.o vt.o \
            sched.o sched_asm.o user.o user_enter.o user_demo_blob.o user_programs.o audio_assets.o shell_blob.o boot_assets.o boot.o gdt_flush.o isr_asm.o \
            sha256.o sha1.o aes.o bn.o rsa.o x25519.o gcm.o tls.o
	$(CC) -T kernel/linker.ld -o kernel.bin -ffreestanding -O0 -nostdlib \
	      -fno-pie -no-pie boot.o kernel.o device.o speaker.o playback.o hda.o e1000.o virtio_net.o net_drv.o net.o vga.o framebuffer.o gpu.o keyboard.o input.o mouse.o shm.o msgq.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs.o install.o diskfmt.o audio_assets_gen.o icon_assets_gen.o icon_assets.o vfs.o fd.o persistfs.o fat32.o exfat.o ntfs.o tty.o syscall.o console.o serial.o power.o vt.o \
	      gdt.o idt.o isr.o pic.o lapic.o ioapic.o irq_controller.o acpi.o pmm.o heap.o vmm.o pf.o \
	      bootstage.o splash.o sched.o sched_asm.o user.o user_enter.o user_demo_blob.o user_programs.o audio_assets.o shell_blob.o boot_assets.o gdt_flush.o isr_asm.o \
	      sha256.o sha1.o aes.o bn.o rsa.o x25519.o gcm.o tls.o -lgcc

kernel.iso: kernel.bin
	mkdir -p isodir/boot/grub
	mkdir -p isodir/EFI/BOOT
	cp kernel.bin isodir/boot/kernel.bin
	cp boot/grub/grub.cfg isodir/boot/grub/grub.cfg
	grub-mkstandalone -O x86_64-efi -o isodir/EFI/BOOT/BOOTX64.EFI "boot/grub/grub.cfg=boot/grub/grub.cfg"
	grub-mkrescue -o /tmp/kernel.iso isodir
	# Docker Desktop 9p: EEXIST from lingering cache; mv fails if host holds lock
	cp /tmp/kernel.iso $@.tmp
	mv -f $@.tmp $@

kernel-usb.img: kernel.bin
	mkdir -p usbroot/EFI/BOOT
	mkdir -p usbroot/boot/grub
	cp kernel.bin usbroot/boot/kernel.bin
	cp boot/grub/grub-usb.cfg usbroot/boot/grub/grub.cfg
	grub-mkstandalone -O x86_64-efi -o usbroot/EFI/BOOT/BOOTX64.EFI "boot/grub/grub.cfg=boot/grub/grub-usb.cfg"
	rm -f kernel-usb.img
	dd if=/dev/zero of=kernel-usb.img bs=1M count=256
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
	@echo Cleaning Windows build artifacts (best-effort; locked files skipped)
	del /f /q kernel.iso kernel.bin kernel-usb.img *.ppm *.log qemu*.log m*.ppm q.log gui-cursor.txt *.tmp *.new 2>nul
	rmdir /s /q isodir usbroot 2>nul || echo " (dirs may be in use)"
else
	rm -f *.o *.ppm *.log kernel.bin kernel.iso kernel-usb.img qemu-smoke.log q.log m*.ppm gui-cursor.txt *.tmp *.new
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
