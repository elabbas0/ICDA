CC  = gcc
ASM = nasm
QEMU = qemu-system-x86_64
OVMF_CODE = /usr/share/OVMF/OVMF_CODE.fd
DOCKER_IMAGE = icda-toolchain
DOCKER_RUN = docker run --rm -v "$(CURDIR):/workspace" -w /workspace $(DOCKER_IMAGE)
SGDISK ?= /usr/sbin/sgdisk



CI_SELFTEST ?= 0
CI_IMAGE ?= 0
SERIAL_VERBOSE ?= 0
SERIAL_SHELL_MIRROR ?= 0


AUDIO_WAVS := $(wildcard userspace/boot.wav userspace/chime.wav userspace/melody.wav userspace/hava_clip.wav)
ICON_ICOS := $(wildcard resources/icons/*.ico)

CFLAGS = -ffreestanding -O0 -Wall -Wextra -fno-exceptions -fno-pie -no-pie \
         -fno-asynchronous-unwind-tables -Ikernel -I. -fno-stack-protector \
         -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -mno-red-zone \
         -DSERIAL_SHELL_MIRROR=$(SERIAL_SHELL_MIRROR) \
         -DCI_SELFTEST=$(CI_SELFTEST) -DCI_IMAGE=$(CI_IMAGE) \
         -DSERIAL_VERBOSE=$(SERIAL_VERBOSE)






USR_CFLAGS = -ffreestanding -O2 -Wall -Wextra -Wpedantic -Wno-unused-command-line-argument -fno-pie -no-pie -mcmodel=large \
             -fno-asynchronous-unwind-tables -fno-stack-protector \
             -msse2 -mfpmath=sse -Iuserspace -I.



IC_MODULES = ic_time ic_anim ic_gfx ic_font ic_theme ic_ui ic_symbols ic_app ic_mem
IC_MODULE_OBJS = $(addsuffix .o,$(IC_MODULES))
IC_HEADERS = userspace/libicda.h userspace/ic_mem.h userspace/ic_time.h userspace/ic_anim.h userspace/ic_gfx.h \
             userspace/ic_font.h userspace/ic_fonts_gen.h userspace/ic_theme.h userspace/ic_ui.h \
             userspace/ic_app.h userspace/gui.h userspace/gui_proto.h \
             userspace/settings_store.h userspace/icda_sys.h

all: kernel.iso kernel-usb.img usb-sync

kernel.o: kernel/kernel.c Makefile kernel/cpu/pat.h kernel/cpu/fpu.h
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

# Wi-Fi: OpenBSD iwm(4) port for the Intel Wireless 8260 (kernel/drivers/net/iwm)
IWM_DIR = kernel/drivers/net/iwm
IWM_HEADERS = $(IWM_DIR)/iwm_compat.h $(IWM_DIR)/iwm_port.h $(IWM_DIR)/net80211.h \
              $(IWM_DIR)/ieee80211.h $(IWM_DIR)/if_iwmreg.h $(IWM_DIR)/if_iwmvar.h \
              $(IWM_DIR)/wpa_crypto.h $(IWM_DIR)/wpa_eapol.h $(IWM_DIR)/wifi.h
# Imported BSD code: silence style warnings that are noise for it.
IWM_CFLAGS = $(CFLAGS) -Wno-sign-compare -Wno-unused-parameter \
             -Wno-unused-but-set-variable -Wno-unused-variable -Wno-unused-function
IWM_OBJS = if_iwm.o iwm_compat.o net80211.o wpa_crypto.o wpa_eapol.o wifi.o wifi_fw_assets.o

if_iwm.o: $(IWM_DIR)/if_iwm.c $(IWM_HEADERS) Makefile
	$(CC) $(IWM_CFLAGS) -c $(IWM_DIR)/if_iwm.c -o if_iwm.o

iwm_compat.o: $(IWM_DIR)/iwm_compat.c $(IWM_HEADERS) Makefile
	$(CC) $(CFLAGS) -c $(IWM_DIR)/iwm_compat.c -o iwm_compat.o

net80211.o: $(IWM_DIR)/net80211.c $(IWM_HEADERS) Makefile
	$(CC) $(CFLAGS) -c $(IWM_DIR)/net80211.c -o net80211.o

wpa_crypto.o: $(IWM_DIR)/wpa_crypto.c $(IWM_DIR)/wpa_crypto.h kernel/crypto/sha1.h kernel/crypto/aes.h
	$(CC) $(CFLAGS) -c $(IWM_DIR)/wpa_crypto.c -o wpa_crypto.o

wpa_eapol.o: $(IWM_DIR)/wpa_eapol.c $(IWM_DIR)/wpa_eapol.h $(IWM_DIR)/wpa_crypto.h
	$(CC) $(CFLAGS) -c $(IWM_DIR)/wpa_eapol.c -o wpa_eapol.o

wifi.o: $(IWM_DIR)/wifi.c $(IWM_HEADERS) kernel/net/net.h kernel/fs/vfs.h Makefile
	$(CC) $(IWM_CFLAGS) -c $(IWM_DIR)/wifi.c -o wifi.o

wifi_fw_assets.o: kernel/proc/wifi_fw_assets.asm resources/firmware/iwlwifi-8000C-36.ucode
	$(ASM) -f elf64 kernel/proc/wifi_fw_assets.asm -o wifi_fw_assets.o

# Host unit test for the WPA2 crypto and handshake (802.11i test vectors)
wifi-crypto-test:
	gcc -O2 -Wall -Wextra -Ikernel -o /tmp/icda-wpa-test $(IWM_DIR)/tests/wpa_test.c \
	    $(IWM_DIR)/wpa_crypto.c $(IWM_DIR)/wpa_eapol.c kernel/crypto/sha1.c kernel/crypto/aes.c
	/tmp/icda-wpa-test

# Host simulation of the station stack against a simulated WPA2 AP
IWM_SIM_SRCS = $(IWM_DIR)/tests/net80211_sim.c $(IWM_DIR)/net80211.c $(IWM_DIR)/iwm_compat.c \
               $(IWM_DIR)/wpa_crypto.c $(IWM_DIR)/wpa_eapol.c kernel/crypto/sha1.c kernel/crypto/aes.c
wifi-sim-test:
	gcc -O0 -g -w -c $(IWM_DIR)/tests/host_stubs.c -o /tmp/icda-sim-stubs.o
	gcc -O0 -g -Wall -Wno-unused-parameter -fno-builtin -I$(IWM_DIR)/tests/host -Ikernel \
	    -include $(IWM_DIR)/tests/host/rename.h -o /tmp/icda-wifi-sim $(IWM_SIM_SRCS) /tmp/icda-sim-stubs.o
	/tmp/icda-wifi-sim

sock.o: kernel/net/sock.c kernel/net/sock.h kernel/net/net.h kernel/drivers/net/net_drv.h kernel/syscall/uaccess.h
	$(CC) $(CFLAGS) -c kernel/net/sock.c -o sock.o

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

gpu.o: kernel/drivers/display/gpu.c kernel/drivers/display/gpu.h kernel/drivers/display/framebuffer.h kernel/drivers/display/flip.h
	$(CC) $(CFLAGS) -c kernel/drivers/display/gpu.c -o gpu.o

virtio_gpu.o: kernel/drivers/display/virtio_gpu.c kernel/drivers/display/virtio_gpu.h Makefile \
              kernel/drivers/pci/pci.h kernel/memory/pmm.h kernel/memory/vmm.h \
              kernel/drivers/display/framebuffer.h kernel/drivers/display/gpu.h
	$(CC) $(CFLAGS) -c kernel/drivers/display/virtio_gpu.c -o virtio_gpu.o

flip.o: kernel/drivers/display/flip.c kernel/drivers/display/flip.h
	$(CC) $(CFLAGS) -c kernel/drivers/display/flip.c -o flip.o

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

devops.o: kernel/dev/devops.c kernel/dev/devops.h
	$(CC) $(CFLAGS) -c kernel/dev/devops.c -o devops.o

devnodes.o: kernel/dev/devnodes.c kernel/dev/devops.h kernel/drivers/rtc/rtc.h kernel/drivers/console/console.h kernel/drivers/display/framebuffer.h kernel/drivers/display/gpu.h kernel/drivers/display/vga.h kernel/drivers/input/input.h kernel/drivers/input/mouse.h kernel/fs/vfs.h kernel/memory/vmm.h kernel/cpu/pat.h kernel/proc/sched.h kernel/syscall/syscall.h
	$(CC) $(CFLAGS) -c kernel/dev/devnodes.c -o devnodes.o

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

lx_vm.o: kernel/linux/lx_vm.c kernel/linux/lx_vm.h kernel/proc/process.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/linux/lx_vm.c -o lx_vm.o

lx.o: kernel/linux/lx.c kernel/linux/lx.h kernel/proc/process.h kernel/fs/vfs.h kernel/tty/pty.h kernel/syscall/uaccess.h kernel/linux/lx_vm.h
	$(CC) $(CFLAGS) -c kernel/linux/lx.c -o lx.o

persistfs.o: kernel/fs/persistfs.c kernel/fs/persistfs.h kernel/fs/vfs.h kernel/drivers/storage/ata.h
	$(CC) $(CFLAGS) -c kernel/fs/persistfs.c -o persistfs.o

sysupdate.o: kernel/fs/sysupdate.c kernel/fs/sysupdate.h kernel/fs/fatfs.h kernel/fs/persistfs.h kernel/fs/vfs.h version.h
	$(CC) $(CFLAGS) -c kernel/fs/sysupdate.c -o sysupdate.o

bootlog.o: kernel/fs/bootlog.c kernel/fs/bootlog.h kernel/fs/fatfs.h kernel/fs/persistfs.h
	$(CC) $(CFLAGS) -c kernel/fs/bootlog.c -o bootlog.o

fat32.o: kernel/fs/fat32.c kernel/fs/fat32.h kernel/fs/vfs.h kernel/drivers/storage/partition.h
	$(CC) $(CFLAGS) -c kernel/fs/fat32.c -o fat32.o

fatfs.o: kernel/fs/fatfs.c kernel/fs/fatfs.h kernel/drivers/storage/partition.h
	$(CC) $(CFLAGS) -c kernel/fs/fatfs.c -o fatfs.o

exfatfs.o: kernel/fs/exfatfs.c kernel/fs/exfatfs.h
	$(CC) $(CFLAGS) -c kernel/fs/exfatfs.c -o exfatfs.o

ntfsfs.o: kernel/fs/ntfsfs.c kernel/fs/ntfsfs.h
	$(CC) $(CFLAGS) -c kernel/fs/ntfsfs.c -o ntfsfs.o

volumes.o: kernel/fs/volumes.c kernel/fs/volumes.h kernel/fs/fatfs.h kernel/fs/exfatfs.h kernel/fs/ntfsfs.h kernel/fs/vfs.h
	$(CC) $(CFLAGS) -c kernel/fs/volumes.c -o volumes.o

exfat.o: kernel/fs/exfat.c kernel/fs/exfat.h kernel/fs/vfs.h kernel/drivers/storage/partition.h
	$(CC) $(CFLAGS) -c kernel/fs/exfat.c -o exfat.o

ntfs.o: kernel/fs/ntfs.c kernel/fs/ntfs.h kernel/fs/vfs.h kernel/drivers/storage/partition.h
	$(CC) $(CFLAGS) -c kernel/fs/ntfs.c -o ntfs.o

pty.o: kernel/tty/pty.c kernel/tty/pty.h kernel/proc/process.h kernel/proc/sched.h
	$(CC) $(CFLAGS) -c kernel/tty/pty.c -o pty.o

tty.o: kernel/tty/tty.c kernel/tty/tty.h kernel/drivers/console/console.h \
       kernel/drivers/input/input.h kernel/memory/heap.h kernel/memory/pmm.h kernel/syscall/syscall.h
	$(CC) $(CFLAGS) -c kernel/tty/tty.c -o tty.o

vt.o: kernel/vt/vt.c kernel/vt/vt.h kernel/proc/sched.h kernel/drivers/console/console.h
	$(CC) $(CFLAGS) -c kernel/vt/vt.c -o vt.o

syscall.o: kernel/syscall/syscall.c kernel/syscall/syscall.h kernel/dev/devops.h kernel/fs/vfs.h kernel/proc/sched.h kernel/fs/install.h kernel/fs/diskfmt.h kernel/net/net.h kernel/memory/pmm.h kernel/memory/vmm.h
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

tsc.o: kernel/cpu/tsc.c kernel/cpu/tsc.h
	$(CC) $(CFLAGS) -c kernel/cpu/tsc.c -o tsc.o

smp.o: kernel/cpu/smp.c kernel/cpu/smp.h kernel/cpu/lapic.h kernel/cpu/gdt.h
	$(CC) $(CFLAGS) -c kernel/cpu/smp.c -o smp.o

ap_trampoline.bin: kernel/cpu/ap_trampoline.asm
	$(ASM) -f bin kernel/cpu/ap_trampoline.asm -o ap_trampoline.bin

ap_blob.o: kernel/cpu/ap_blob.asm ap_trampoline.bin
	$(ASM) -f elf64 kernel/cpu/ap_blob.asm -o ap_blob.o

pat.o: kernel/cpu/pat.c kernel/cpu/pat.h kernel/drivers/serial/serial.h \
       kernel/drivers/console/console.h
	$(CC) $(CFLAGS) -c kernel/cpu/pat.c -o pat.o

fpu.o: kernel/cpu/fpu.c kernel/cpu/fpu.h
	$(CC) $(CFLAGS) -c kernel/cpu/fpu.c -o fpu.o

rtc.o: kernel/drivers/rtc/rtc.c kernel/drivers/rtc/rtc.h
	$(CC) $(CFLAGS) -c kernel/drivers/rtc/rtc.c -o rtc.o

ioapic.o: kernel/cpu/ioapic.c kernel/cpu/ioapic.h kernel/firmware/acpi.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/cpu/ioapic.c -o ioapic.o

irq_controller.o: kernel/cpu/irq_controller.c kernel/cpu/irq_controller.h kernel/cpu/pic.h
	$(CC) $(CFLAGS) -c kernel/cpu/irq_controller.c -o irq_controller.o

acpi.o: kernel/firmware/acpi.c kernel/firmware/acpi.h kernel/cpu/multiboot2.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/firmware/acpi.c -o acpi.o

efi.o: kernel/firmware/efi.c kernel/firmware/efi.h kernel/cpu/multiboot2.h kernel/memory/vmm.h
	$(CC) $(CFLAGS) -c kernel/firmware/efi.c -o efi.o

boot.o: kernel/boot.asm
	$(ASM) -f elf64 kernel/boot.asm -o boot.o

bootstage.o: kernel/diag/bootstage.c kernel/diag/bootstage.h \
             kernel/drivers/display/framebuffer.h kernel/drivers/display/vga.h \
             kernel/drivers/serial/serial.h
	$(CC) $(CFLAGS) -c kernel/diag/bootstage.c -o bootstage.o

splash.o: kernel/diag/splash.c kernel/diag/splash.h \
          kernel/drivers/display/framebuffer.h kernel/drivers/display/font.h version.h
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
       kernel/cpu/multiboot2.h kernel/cpu/pat.h kernel/drivers/display/framebuffer.h
	$(CC) $(CFLAGS) -c kernel/memory/vmm.c -o vmm.o

pf.o: kernel/memory/pf.c kernel/memory/pf.h kernel/memory/vmm.h \
      kernel/memory/pmm.h kernel/cpu/isr.h kernel/drivers/display/framebuffer.h
	$(CC) $(CFLAGS) -c kernel/memory/pf.c -o pf.o

sched.o: kernel/proc/sched.c kernel/proc/sched.h kernel/proc/process.h \
         kernel/memory/pmm.h kernel/memory/vmm.h kernel/memory/pf.h \
         kernel/cpu/gdt.h kernel/cpu/fpu.h kernel/drivers/display/framebuffer.h
	$(CC) $(CFLAGS) -c kernel/proc/sched.c -o sched.o

sched_asm.o: kernel/proc/sched.asm
	$(ASM) -f elf64 kernel/proc/sched.asm -o sched_asm.o

user.o: kernel/proc/user.c kernel/proc/user.h kernel/proc/process.h kernel/proc/elf.h kernel/memory/vmm.h kernel/memory/pf.h
	$(CC) $(CFLAGS) -c kernel/proc/user.c -o user.o

user_enter.o: kernel/proc/user_enter.asm
	$(ASM) -f elf64 kernel/proc/user_enter.asm -o user_enter.o

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

LIBC_HEADERS = userspace/libc/include/stdio.h userspace/libc/include/stdlib.h \
               userspace/libc/include/string.h userspace/libc/include/ctype.h
LIBC_CFLAGS = $(USR_CFLAGS) -fno-builtin -fno-tree-loop-distribute-patterns

libc_core.o: userspace/libc/libc.c $(LIBC_HEADERS) userspace/icda_sys.h userspace/ic_mem.h
	$(CC) $(LIBC_CFLAGS) -c userspace/libc/libc.c -o /tmp/icda-libc_core.o
	cp -f /tmp/icda-libc_core.o libc_core.o

libc.o: libc_core.o ic_mem.o
	ld -r -o /tmp/icda-libc.o libc_core.o ic_mem.o
	cp -f /tmp/icda-libc.o libc.o

crt1.o: userspace/libc/crt1.asm
	$(ASM) -f elf64 userspace/libc/crt1.asm -o crt1.o

libctest.o: userspace/libctest.c $(LIBC_HEADERS)
	$(CC) $(USR_CFLAGS) -Iuserspace/libc/include -c userspace/libctest.c -o /tmp/icda-libctest.o
	cp -f /tmp/icda-libctest.o libctest.o

userspace/libctest.elf: crt1.o libctest.o libc.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-libctest.elf crt1.o libctest.o libc.o
	printf '\377' | dd of=/tmp/icda-libctest.elf bs=1 seek=7 conv=notrunc status=none
	cp -f /tmp/icda-libctest.elf userspace/libctest.elf

# Surfer: native web browser.  Its network library (sockets, HTTP, TLS 1.3)
# reuses the kernel's crypto sources, compiled for userspace.
SURFER_CFLAGS = $(USR_CFLAGS) -Iuserspace/libc/include -Iuserspace/surfer -Ikernel/crypto -Wno-pedantic
SURFER_HEADERS = $(wildcard userspace/surfer/*.h) $(LIBC_HEADERS) userspace/icda_sys.h
SURFER_NET_OBJS = surfer_net.o surfer_http.o surfer_tls.o surfer_x509.o surfer_ecdsa.o surfer_sha512.o ucrypto_sha256.o ucrypto_sha1.o ucrypto_aes.o \
                  ucrypto_gcm.o ucrypto_bn.o ucrypto_rsa.o ucrypto_x25519.o

surfer_%.o: userspace/surfer/%.c $(SURFER_HEADERS)
	$(CC) $(SURFER_CFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

surfer_%.o: userspace/surfer/crypto/%.c $(SURFER_HEADERS) $(wildcard userspace/surfer/crypto/*.h)
	$(CC) $(SURFER_CFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

ucrypto_%.o: kernel/crypto/%.c $(wildcard kernel/crypto/*.h)
	$(CC) $(SURFER_CFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

updated.o: userspace/updated.c $(SURFER_HEADERS) userspace/surfer/crypto/ed25519.h
	$(CC) $(SURFER_CFLAGS) -c userspace/updated.c -o /tmp/icda-updated.o
	cp -f /tmp/icda-updated.o updated.o

# OTA patch daemon (/sbin/updated)
userspace/updated.elf: crt1.o updated.o surfer_ed25519.o $(SURFER_NET_OBJS) libc.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-updated.elf crt1.o updated.o surfer_ed25519.o $(SURFER_NET_OBJS) libc.o
	printf '\377' | dd of=/tmp/icda-updated.elf bs=1 seek=7 conv=notrunc status=none
	cp -f /tmp/icda-updated.elf userspace/updated.elf

userspace/fetch.elf: crt1.o surfer_fetch.o $(SURFER_NET_OBJS) libc.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-fetch.elf crt1.o surfer_fetch.o $(SURFER_NET_OBJS) libc.o
	printf '\377' | dd of=/tmp/icda-fetch.elf bs=1 seek=7 conv=notrunc status=none
	cp -f /tmp/icda-fetch.elf userspace/fetch.elf

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
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/shell.c -o /tmp/icda-shell.o
	cp -f /tmp/icda-shell.o shell.o

audioplay.o: userspace/audioplay.c userspace/gui.h userspace/gui_proto.h $(IC_HEADERS) userspace/icda_sys.h \
             userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/audioplay.c -o /tmp/icda-audioplay.o
	cp -f /tmp/icda-audioplay.o audioplay.o

editor.o: userspace/editor.c userspace/gui.h userspace/gui_proto.h $(IC_HEADERS) userspace/icda_sys.h \
          userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/editor.c -o /tmp/icda-editor.o
	cp -f /tmp/icda-editor.o editor.o

diskman.o: userspace/diskman.c userspace/gui.h userspace/gui_proto.h $(IC_HEADERS) userspace/icda_sys.h \
           userspace/ic_version.h version.h
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
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-diskman.app crt0.o diskman.o gui.o libicda.o
	cp -f /tmp/icda-diskman.app userspace/diskman.app

userspace/taskman.app: crt0.o taskman.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-taskman.app crt0.o taskman.o gui.o libicda.o
	cp -f /tmp/icda-taskman.app userspace/taskman.app

taskman.o: userspace/taskman.c userspace/gui.h userspace/gui_proto.h $(IC_HEADERS) userspace/font.h userspace/icda_sys.h \
           userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/taskman.c -o /tmp/icda-taskman.o
	cp -f /tmp/icda-taskman.o taskman.o

browser_start.o: userspace/browser_start.asm
	$(ASM) -f elf64 userspace/browser_start.asm -o /tmp/icda-browser_start.o
	cp -f /tmp/icda-browser_start.o browser_start.o

browser.o: userspace/browser.c userspace/gui.h userspace/gui_proto.h $(IC_HEADERS) userspace/icda_sys.h \
           userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/browser.c -o /tmp/icda-browser.o
	cp -f /tmp/icda-browser.o browser.o

SURFER_ENGINE_OBJS = surfer_html.o surfer_css.o surfer_font.o surfer_layout.o surfer_paint.o surfer_image.o surfer_form.o surfer_download.o \
                     surfer_js.o surfer_prelude.o $(QJS_OBJS) $(LIBM_OBJS) $(WEBP_OBJS)

# libwebp decoder (third_party/libwebp, BSD): lossy, lossless and animated
# WebP for Surfer and Media.
WEBP_DIR = userspace/surfer/third_party/libwebp
WEBP_SRCS = $(wildcard $(WEBP_DIR)/src/*/*.c)
WEBP_OBJS = $(patsubst %.c,webp_%.o,$(notdir $(WEBP_SRCS)))
WEBP_CFLAGS = $(USR_CFLAGS) -Iuserspace/libc/include -Iuserspace/media/compat -I$(WEBP_DIR) -DNDEBUG -w
vpath %.c $(WEBP_DIR)/src/dec $(WEBP_DIR)/src/dsp $(WEBP_DIR)/src/utils $(WEBP_DIR)/src/demux
webp_%.o: %.c $(wildcard $(WEBP_DIR)/src/*/*.h)
	$(CC) $(WEBP_CFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

surfer_image.o: userspace/surfer/image.c $(SURFER_HEADERS) $(wildcard $(WEBP_DIR)/src/webp/*.h)
	$(CC) $(SURFER_CFLAGS) -I$(WEBP_DIR) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

# QuickJS (third_party/quickjs, MIT).  -D__ICDA__ drops Atomics (no OS threads).
QJS_OBJS = qjs_quickjs.o qjs_libregexp.o qjs_libunicode.o qjs_cutils.o qjs_dtoa.o
QJS_CFLAGS = $(SURFER_CFLAGS) -D__ICDA__ -DCONFIG_VERSION=\"2025-04-26\" -Dalloca=__builtin_alloca -w
qjs_%.o: userspace/surfer/third_party/quickjs/%.c $(wildcard userspace/surfer/third_party/quickjs/*.h) $(LIBC_HEADERS)
	$(CC) $(QJS_CFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

# libm: double-precision math from musl (userspace/libc/math, MIT).
LIBM_SRCS = $(wildcard userspace/libc/math/*.c)
LIBM_OBJS = $(patsubst userspace/libc/math/%.c,libm_%.o,$(LIBM_SRCS))
libm_%.o: userspace/libc/math/%.c userspace/libc/math/libm.h userspace/libc/include/math.h
	$(CC) $(USR_CFLAGS) -Iuserspace/libc/include -Iuserspace/libc/math -include userspace/libc/math/musl_compat.h -w -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

# The DOM prelude is JavaScript, embedded as a NUL-terminated byte array.
surfer_prelude.o: userspace/surfer/prelude.js
	perl -e 'local $$/; my $$d = <>; print "const char surfer_prelude_js[] = {", join(",", map { my $$c = ord; $$c > 127 ? $$c - 256 : $$c } split //, $$d), ",0};\nconst unsigned surfer_prelude_js_len = ", length($$d), ";\n";' $< > /tmp/icda-prelude_js.c
	$(CC) $(SURFER_CFLAGS) -c /tmp/icda-prelude_js.c -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

surfer_surfer.o: userspace/surfer/surfer.c $(SURFER_HEADERS) $(IC_HEADERS)
	$(CC) $(SURFER_CFLAGS) -Iuserspace -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

# OpenH264 (third_party/openh264, BSD-2-Clause): H.264 decoding for Media.
# C++ without exceptions or RTTI, ICDA's libc behind oh_prefix.h, threads off.
OH_DIR = userspace/media/third_party/openh264/codec
OH_CXXFLAGS = -ffreestanding -O2 -fno-pie -no-pie -mcmodel=large -fno-asynchronous-unwind-tables -fno-stack-protector \
              -msse2 -mfpmath=sse -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -nostdinc++ \
              -Iuserspace/media/compat_oh -Iuserspace/media/compat -Iuserspace/libc/include -Iuserspace -Iuserspace/media \
              -I$(OH_DIR)/api/wels -I$(OH_DIR)/common/inc -I$(OH_DIR)/decoder/core/inc -I$(OH_DIR)/decoder/plus/inc \
              -include userspace/media/compat_oh/oh_prefix.h -DNDEBUG -DX86_ASM -DHAVE_AVX2 -w
OH_COMMON = common_tables copy_mb cpu crt_util_safe_x deblocking_common expand_pic intra_pred_common mc memory_align \
            sad_common utils welsCodecTrace WelsThreadLib
OH_DECODER = au_parser bit_stream cabac_decoder deblocking decode_mb_aux decode_slice decoder decoder_core \
             decoder_data_tables error_concealment fmo get_intra_predictor manage_dec_ref memmgr_nal_unit mv_pred \
             parse_mb_syn_cabac parse_mb_syn_cavlc pic_queue rec_mb wels_decoder_thread
OH_ASM_COMMON = cpuid dct deblock expand_picture intra_pred_com mb_copy mc_chroma mc_luma satd_sad vaa
OH_ASM_DECODER = dct intra_pred
OH_OBJS = $(patsubst %,ohc_%.o,$(OH_COMMON)) $(patsubst %,ohd_%.o,$(OH_DECODER)) ohp_welsDecoderExt.o \
          $(patsubst %,ohac_%.o,$(OH_ASM_COMMON)) $(patsubst %,ohad_%.o,$(OH_ASM_DECODER))
OH_HEADERS = $(wildcard $(OH_DIR)/api/wels/*.h $(OH_DIR)/common/inc/*.h $(OH_DIR)/decoder/core/inc/*.h \
             $(OH_DIR)/decoder/plus/inc/*.h userspace/media/compat_oh/*.h)

ohc_%.o: $(OH_DIR)/common/src/%.cpp $(OH_HEADERS)
	g++ $(OH_CXXFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@
ohd_%.o: $(OH_DIR)/decoder/core/src/%.cpp $(OH_HEADERS)
	g++ $(OH_CXXFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@
ohp_%.o: $(OH_DIR)/decoder/plus/src/%.cpp $(OH_HEADERS)
	g++ $(OH_CXXFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@
ohac_%.o: $(OH_DIR)/common/x86/%.asm $(OH_DIR)/common/x86/asm_inc.asm
	$(ASM) -f elf64 -DUNIX64 -DHAVE_AVX2 -I$(OH_DIR)/common/x86/ $< -o $@
ohad_%.o: $(OH_DIR)/decoder/core/x86/%.asm $(OH_DIR)/common/x86/asm_inc.asm
	$(ASM) -f elf64 -DUNIX64 -DHAVE_AVX2 -I$(OH_DIR)/common/x86/ $< -o $@
media_h264.o: userspace/media/h264.cpp userspace/media/h264.h $(OH_HEADERS)
	g++ $(OH_CXXFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

# libvpx (third_party/libvpx, BSD): VP8 / VP9 decoding.  gen/ holds what libvpx's
# configure produced for x86_64 decoders only (runtime CPU detection, no threads).
VPX_DIR = userspace/media/third_party/libvpx
VPX_SRCS =
VPX_SRCS += vp8/common/alloccommon.c
VPX_SRCS += vp8/common/blockd.c
VPX_SRCS += vp8/common/dequantize.c
VPX_SRCS += vp8/common/entropy.c
VPX_SRCS += vp8/common/entropymode.c
VPX_SRCS += vp8/common/entropymv.c
VPX_SRCS += vp8/common/extend.c
VPX_SRCS += vp8/common/filter.c
VPX_SRCS += vp8/common/findnearmv.c
VPX_SRCS += vp8/common/generic/systemdependent.c
VPX_SRCS += vp8/common/idct_blk.c
VPX_SRCS += vp8/common/idctllm.c
VPX_SRCS += vp8/common/loopfilter_filters.c
VPX_SRCS += vp8/common/mbpitch.c
VPX_SRCS += vp8/common/modecont.c
VPX_SRCS += vp8/common/quant_common.c
VPX_SRCS += vp8/common/reconinter.c
VPX_SRCS += vp8/common/reconintra.c
VPX_SRCS += vp8/common/reconintra4x4.c
VPX_SRCS += vp8/common/rtcd.c
VPX_SRCS += vp8/common/setupintrarecon.c
VPX_SRCS += vp8/common/swapyv12buffer.c
VPX_SRCS += vp8/common/treecoder.c
VPX_SRCS += vp8/common/vp8_loopfilter.c
VPX_SRCS += vp8/common/x86/bilinear_filter_sse2.c
VPX_SRCS += vp8/common/x86/dequantize_mmx.asm
VPX_SRCS += vp8/common/x86/idct_blk_mmx.c
VPX_SRCS += vp8/common/x86/idct_blk_sse2.c
VPX_SRCS += vp8/common/x86/idctllm_mmx.asm
VPX_SRCS += vp8/common/x86/idctllm_sse2.asm
VPX_SRCS += vp8/common/x86/iwalsh_sse2.asm
VPX_SRCS += vp8/common/x86/loopfilter_block_sse2_x86_64.asm
VPX_SRCS += vp8/common/x86/loopfilter_sse2.asm
VPX_SRCS += vp8/common/x86/loopfilter_x86.c
VPX_SRCS += vp8/common/x86/recon_mmx.asm
VPX_SRCS += vp8/common/x86/recon_sse2.asm
VPX_SRCS += vp8/common/x86/subpixel_mmx.asm
VPX_SRCS += vp8/common/x86/subpixel_sse2.asm
VPX_SRCS += vp8/common/x86/subpixel_ssse3.asm
VPX_SRCS += vp8/common/x86/vp8_asm_stubs.c
VPX_SRCS += vp8/decoder/dboolhuff.c
VPX_SRCS += vp8/decoder/decodeframe.c
VPX_SRCS += vp8/decoder/decodemv.c
VPX_SRCS += vp8/decoder/detokenize.c
VPX_SRCS += vp8/decoder/onyxd_if.c
VPX_SRCS += vp8/vp8_dx_iface.c
VPX_SRCS += vp9/common/vp9_alloccommon.c
VPX_SRCS += vp9/common/vp9_blockd.c
VPX_SRCS += vp9/common/vp9_common_data.c
VPX_SRCS += vp9/common/vp9_entropy.c
VPX_SRCS += vp9/common/vp9_entropymode.c
VPX_SRCS += vp9/common/vp9_entropymv.c
VPX_SRCS += vp9/common/vp9_filter.c
VPX_SRCS += vp9/common/vp9_frame_buffers.c
VPX_SRCS += vp9/common/vp9_idct.c
VPX_SRCS += vp9/common/vp9_loopfilter.c
VPX_SRCS += vp9/common/vp9_mvref_common.c
VPX_SRCS += vp9/common/vp9_pred_common.c
VPX_SRCS += vp9/common/vp9_quant_common.c
VPX_SRCS += vp9/common/vp9_reconinter.c
VPX_SRCS += vp9/common/vp9_reconintra.c
VPX_SRCS += vp9/common/vp9_rtcd.c
VPX_SRCS += vp9/common/vp9_scale.c
VPX_SRCS += vp9/common/vp9_scan.c
VPX_SRCS += vp9/common/vp9_seg_common.c
VPX_SRCS += vp9/common/vp9_thread_common.c
VPX_SRCS += vp9/common/vp9_tile_common.c
VPX_SRCS += vp9/common/x86/vp9_idct_intrin_sse2.c
VPX_SRCS += vp9/decoder/vp9_decodeframe.c
VPX_SRCS += vp9/decoder/vp9_decodemv.c
VPX_SRCS += vp9/decoder/vp9_decoder.c
VPX_SRCS += vp9/decoder/vp9_detokenize.c
VPX_SRCS += vp9/decoder/vp9_dsubexp.c
VPX_SRCS += vp9/decoder/vp9_job_queue.c
VPX_SRCS += vp9/vp9_dx_iface.c
VPX_SRCS += vp9/vp9_iface_common.c
VPX_SRCS += vpx/src/vpx_codec.c
VPX_SRCS += vpx/src/vpx_decoder.c
VPX_SRCS += vpx/src/vpx_image.c
VPX_SRCS += vpx_dsp/bitreader.c
VPX_SRCS += vpx_dsp/bitreader_buffer.c
VPX_SRCS += vpx_dsp/intrapred.c
VPX_SRCS += vpx_dsp/inv_txfm.c
VPX_SRCS += vpx_dsp/loopfilter.c
VPX_SRCS += vpx_dsp/prob.c
VPX_SRCS += vpx_dsp/vpx_convolve.c
VPX_SRCS += vpx_dsp/vpx_dsp_rtcd.c
VPX_SRCS += vpx_dsp/x86/intrapred_sse2.asm
VPX_SRCS += vpx_dsp/x86/intrapred_ssse3.asm
VPX_SRCS += vpx_dsp/x86/inv_txfm_avx2.c
VPX_SRCS += vpx_dsp/x86/inv_txfm_sse2.c
VPX_SRCS += vpx_dsp/x86/inv_txfm_ssse3.c
VPX_SRCS += vpx_dsp/x86/inv_wht_sse2.asm
VPX_SRCS += vpx_dsp/x86/loopfilter_avx2.c
VPX_SRCS += vpx_dsp/x86/loopfilter_sse2.c
VPX_SRCS += vpx_dsp/x86/vpx_convolve_copy_sse2.asm
VPX_SRCS += vpx_dsp/x86/vpx_subpixel_4t_intrin_sse2.c
VPX_SRCS += vpx_dsp/x86/vpx_subpixel_8t_intrin_avx2.c
VPX_SRCS += vpx_dsp/x86/vpx_subpixel_8t_intrin_ssse3.c
VPX_SRCS += vpx_dsp/x86/vpx_subpixel_8t_sse2.asm
VPX_SRCS += vpx_dsp/x86/vpx_subpixel_8t_ssse3.asm
VPX_SRCS += vpx_dsp/x86/vpx_subpixel_bilinear_sse2.asm
VPX_SRCS += vpx_dsp/x86/vpx_subpixel_bilinear_ssse3.asm
VPX_SRCS += vpx_mem/vpx_mem.c
VPX_SRCS += vpx_ports/emms_mmx.asm
VPX_SRCS += vpx_scale/generic/gen_scalers.c
VPX_SRCS += vpx_scale/generic/vpx_scale.c
VPX_SRCS += vpx_scale/generic/yv12config.c
VPX_SRCS += vpx_scale/generic/yv12extend.c
VPX_SRCS += vpx_scale/vpx_scale_rtcd.c
VPX_SRCS += vpx_util/vpx_thread.c
VPX_SRCS += gen/vpx_config.c

VPX_CFLAGS = $(USR_CFLAGS) -std=gnu99 -Iuserspace/media/compat_vpx -Iuserspace/media/compat -Iuserspace/libc/include \
             -I$(VPX_DIR)/gen -I$(VPX_DIR) -DNDEBUG -w
vpx_obj = vpx_$(subst /,_,$(basename $(1))).o
vpx_simd = $(if $(findstring _ssse3,$(1)),-mssse3,$(if $(findstring _sse4,$(1)),-msse4.1,$(if $(findstring _avx2,$(1)),-mavx2,$(if $(findstring _avx,$(1)),-mavx,))))
define VPX_RULE
$(call vpx_obj,$(1)): $(VPX_DIR)/$(1)
ifeq ($(suffix $(1)),.asm)
	$$(ASM) -f elf64 -I$(VPX_DIR)/gen/ -I$(VPX_DIR)/ $$< -o $$@
else
	$$(CC) $$(VPX_CFLAGS) $(call vpx_simd,$(1)) -c $$< -o /tmp/icda-$$@
	cp -f /tmp/icda-$$@ $$@
endif
endef
$(foreach s,$(VPX_SRCS),$(eval $(call VPX_RULE,$(s))))
VPX_OBJS = $(foreach s,$(VPX_SRCS),$(call vpx_obj,$(s)))
libvpx_icda.a: $(VPX_OBJS)
	rm -f $@ && ar rcs $@ $^

# libopus (third_party/opus, BSD): Opus soundtracks (WebM, YouTube).
OPUS_DIR = userspace/media/third_party/opus
OPUS_SRCS =
OPUS_SRCS += celt/bands.c
OPUS_SRCS += celt/celt.c
OPUS_SRCS += celt/celt_encoder.c
OPUS_SRCS += celt/celt_decoder.c
OPUS_SRCS += celt/cwrs.c
OPUS_SRCS += celt/entcode.c
OPUS_SRCS += celt/entdec.c
OPUS_SRCS += celt/entenc.c
OPUS_SRCS += celt/kiss_fft.c
OPUS_SRCS += celt/laplace.c
OPUS_SRCS += celt/mathops.c
OPUS_SRCS += celt/mdct.c
OPUS_SRCS += celt/modes.c
OPUS_SRCS += celt/pitch.c
OPUS_SRCS += celt/celt_lpc.c
OPUS_SRCS += celt/quant_bands.c
OPUS_SRCS += celt/rate.c
OPUS_SRCS += celt/vq.c
OPUS_SRCS += silk/CNG.c
OPUS_SRCS += silk/code_signs.c
OPUS_SRCS += silk/init_decoder.c
OPUS_SRCS += silk/decode_core.c
OPUS_SRCS += silk/decode_frame.c
OPUS_SRCS += silk/decode_parameters.c
OPUS_SRCS += silk/decode_indices.c
OPUS_SRCS += silk/decode_pulses.c
OPUS_SRCS += silk/decoder_set_fs.c
OPUS_SRCS += silk/dec_API.c
OPUS_SRCS += silk/enc_API.c
OPUS_SRCS += silk/encode_indices.c
OPUS_SRCS += silk/encode_pulses.c
OPUS_SRCS += silk/gain_quant.c
OPUS_SRCS += silk/interpolate.c
OPUS_SRCS += silk/LP_variable_cutoff.c
OPUS_SRCS += silk/NLSF_decode.c
OPUS_SRCS += silk/NSQ.c
OPUS_SRCS += silk/NSQ_del_dec.c
OPUS_SRCS += silk/PLC.c
OPUS_SRCS += silk/shell_coder.c
OPUS_SRCS += silk/tables_gain.c
OPUS_SRCS += silk/tables_LTP.c
OPUS_SRCS += silk/tables_NLSF_CB_NB_MB.c
OPUS_SRCS += silk/tables_NLSF_CB_WB.c
OPUS_SRCS += silk/tables_other.c
OPUS_SRCS += silk/tables_pitch_lag.c
OPUS_SRCS += silk/tables_pulses_per_block.c
OPUS_SRCS += silk/VAD.c
OPUS_SRCS += silk/control_audio_bandwidth.c
OPUS_SRCS += silk/quant_LTP_gains.c
OPUS_SRCS += silk/VQ_WMat_EC.c
OPUS_SRCS += silk/HP_variable_cutoff.c
OPUS_SRCS += silk/NLSF_encode.c
OPUS_SRCS += silk/NLSF_VQ.c
OPUS_SRCS += silk/NLSF_unpack.c
OPUS_SRCS += silk/NLSF_del_dec_quant.c
OPUS_SRCS += silk/process_NLSFs.c
OPUS_SRCS += silk/stereo_LR_to_MS.c
OPUS_SRCS += silk/stereo_MS_to_LR.c
OPUS_SRCS += silk/check_control_input.c
OPUS_SRCS += silk/control_SNR.c
OPUS_SRCS += silk/init_encoder.c
OPUS_SRCS += silk/control_codec.c
OPUS_SRCS += silk/A2NLSF.c
OPUS_SRCS += silk/ana_filt_bank_1.c
OPUS_SRCS += silk/biquad_alt.c
OPUS_SRCS += silk/bwexpander_32.c
OPUS_SRCS += silk/bwexpander.c
OPUS_SRCS += silk/debug.c
OPUS_SRCS += silk/decode_pitch.c
OPUS_SRCS += silk/inner_prod_aligned.c
OPUS_SRCS += silk/lin2log.c
OPUS_SRCS += silk/log2lin.c
OPUS_SRCS += silk/LPC_analysis_filter.c
OPUS_SRCS += silk/LPC_inv_pred_gain.c
OPUS_SRCS += silk/table_LSF_cos.c
OPUS_SRCS += silk/NLSF2A.c
OPUS_SRCS += silk/NLSF_stabilize.c
OPUS_SRCS += silk/NLSF_VQ_weights_laroia.c
OPUS_SRCS += silk/pitch_est_tables.c
OPUS_SRCS += silk/resampler.c
OPUS_SRCS += silk/resampler_down2_3.c
OPUS_SRCS += silk/resampler_down2.c
OPUS_SRCS += silk/resampler_private_AR2.c
OPUS_SRCS += silk/resampler_private_down_FIR.c
OPUS_SRCS += silk/resampler_private_IIR_FIR.c
OPUS_SRCS += silk/resampler_private_up2_HQ.c
OPUS_SRCS += silk/resampler_rom.c
OPUS_SRCS += silk/sigm_Q15.c
OPUS_SRCS += silk/sort.c
OPUS_SRCS += silk/sum_sqr_shift.c
OPUS_SRCS += silk/stereo_decode_pred.c
OPUS_SRCS += silk/stereo_encode_pred.c
OPUS_SRCS += silk/stereo_find_predictor.c
OPUS_SRCS += silk/stereo_quant_pred.c
OPUS_SRCS += silk/LPC_fit.c
OPUS_SRCS += silk/float/apply_sine_window_FLP.c
OPUS_SRCS += silk/float/corrMatrix_FLP.c
OPUS_SRCS += silk/float/encode_frame_FLP.c
OPUS_SRCS += silk/float/find_LPC_FLP.c
OPUS_SRCS += silk/float/find_LTP_FLP.c
OPUS_SRCS += silk/float/find_pitch_lags_FLP.c
OPUS_SRCS += silk/float/find_pred_coefs_FLP.c
OPUS_SRCS += silk/float/LPC_analysis_filter_FLP.c
OPUS_SRCS += silk/float/LTP_analysis_filter_FLP.c
OPUS_SRCS += silk/float/LTP_scale_ctrl_FLP.c
OPUS_SRCS += silk/float/noise_shape_analysis_FLP.c
OPUS_SRCS += silk/float/process_gains_FLP.c
OPUS_SRCS += silk/float/regularize_correlations_FLP.c
OPUS_SRCS += silk/float/residual_energy_FLP.c
OPUS_SRCS += silk/float/warped_autocorrelation_FLP.c
OPUS_SRCS += silk/float/wrappers_FLP.c
OPUS_SRCS += silk/float/autocorrelation_FLP.c
OPUS_SRCS += silk/float/burg_modified_FLP.c
OPUS_SRCS += silk/float/bwexpander_FLP.c
OPUS_SRCS += silk/float/energy_FLP.c
OPUS_SRCS += silk/float/inner_product_FLP.c
OPUS_SRCS += silk/float/k2a_FLP.c
OPUS_SRCS += silk/float/LPC_inv_pred_gain_FLP.c
OPUS_SRCS += silk/float/pitch_analysis_core_FLP.c
OPUS_SRCS += silk/float/scale_copy_vector_FLP.c
OPUS_SRCS += silk/float/scale_vector_FLP.c
OPUS_SRCS += silk/float/schur_FLP.c
OPUS_SRCS += silk/float/sort_FLP.c
OPUS_SRCS += src/opus.c
OPUS_SRCS += src/opus_decoder.c
OPUS_SRCS += src/opus_encoder.c
OPUS_SRCS += src/extensions.c
OPUS_SRCS += src/opus_multistream.c
OPUS_SRCS += src/opus_multistream_encoder.c
OPUS_SRCS += src/opus_multistream_decoder.c
OPUS_SRCS += src/repacketizer.c
OPUS_SRCS += src/opus_projection_encoder.c
OPUS_SRCS += src/opus_projection_decoder.c
OPUS_SRCS += src/mapping_matrix.c
OPUS_SRCS += src/analysis.c
OPUS_SRCS += src/mlp.c
OPUS_SRCS += src/mlp_data.c

OPUS_CFLAGS = $(USR_CFLAGS) -Iuserspace/media/compat -Iuserspace/libc/include -I$(OPUS_DIR)/include -I$(OPUS_DIR)/celt \
              -I$(OPUS_DIR)/silk -I$(OPUS_DIR)/silk/float -I$(OPUS_DIR) -DOPUS_BUILD -DUSE_ALLOCA -Dalloca=__builtin_alloca -DNDEBUG -w
opus_obj = opus_$(subst /,_,$(basename $(1))).o
define OPUS_RULE
$(call opus_obj,$(1)): $(OPUS_DIR)/$(1)
	$$(CC) $$(OPUS_CFLAGS) -c $$< -o /tmp/icda-$$@
	cp -f /tmp/icda-$$@ $$@
endef
$(foreach s,$(OPUS_SRCS),$(eval $(call OPUS_RULE,$(s))))
OPUS_OBJS = $(foreach s,$(OPUS_SRCS),$(call opus_obj,$(s)))
libopus_icda.a: $(OPUS_OBJS)
	rm -f $@ && ar rcs $@ $^

media_setjmp.o: userspace/media/setjmp.asm
	$(ASM) -f elf64 $< -o $@

# Media: player and viewer (userspace/media).  The decoder libraries keep
# their own translation units; their static names would clash otherwise.
MEDIA_CFLAGS = $(USR_CFLAGS) -Iuserspace/libc/include -Iuserspace/media/compat -Iuserspace/media -Iuserspace/surfer \
               -Ikernel/crypto -I$(VPX_DIR) -I$(OPUS_DIR)/include -Dalloca=__builtin_alloca -Wno-pedantic
MEDIA_LIB_CFLAGS = $(MEDIA_CFLAGS) -w
MEDIA_HEADERS = $(wildcard userspace/media/*.h) $(LIBC_HEADERS) $(IC_HEADERS) userspace/icda_sys.h userspace/surfer/image.h
MEDIA_OBJS = media_media.o media_video.o media_youtube.o media_adec.o media_aac.o \
             media_impl_drlibs.o media_impl_mp3.o media_impl_vorbis.o media_impl_mp4.o media_mp4demux.o media_mkvdemux.o \
             media_vdec.o media_pdec.o media_cxxrt.o media_h264.o media_setjmp.o $(OH_OBJS)

media_media.o media_video.o media_youtube.o media_aac.o media_mp4demux.o media_mkvdemux.o media_vdec.o media_pdec.o \
           media_cxxrt.o: media_%.o: userspace/media/%.c $(MEDIA_HEADERS)
	$(CC) $(MEDIA_CFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

media_adec.o media_impl_drlibs.o media_impl_mp3.o media_impl_vorbis.o media_impl_mp4.o: media_%.o: userspace/media/%.c $(MEDIA_HEADERS) \
           $(wildcard userspace/media/third_party/*)
	$(CC) $(MEDIA_LIB_CFLAGS) -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

userspace/media.app: crt1.o $(MEDIA_OBJS) libvpx_icda.a libopus_icda.a surfer_image.o $(WEBP_OBJS) $(SURFER_NET_OBJS) $(LIBM_OBJS) gui.o libicda.o libc_core.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-media.app crt1.o $(MEDIA_OBJS) surfer_image.o $(WEBP_OBJS) libvpx_icda.a libopus_icda.a $(SURFER_NET_OBJS) $(LIBM_OBJS) \
	   gui.o libicda.o libc_core.o $(shell $(CC) -print-libgcc-file-name)
	cp -f /tmp/icda-media.app userspace/media.app

# /apps/browser.app is Surfer.  <video> uses Media's demuxers and decoders
# (see userspace/surfer/media_el.c).
SURFER_MEDIA_OBJS = surfer_media_el.o media_mp4demux.o media_mkvdemux.o media_vdec.o media_pdec.o media_aac.o media_cxxrt.o
SURFER_MEDIA_OBJS += media_h264.o media_setjmp.o media_impl_vorbis.o $(OH_OBJS)
surfer_media_el.o: SURFER_CFLAGS += -Iuserspace/media
surfer_media_el.o: $(wildcard userspace/media/*.h)

userspace/browser.app: crt1.o surfer_surfer.o $(SURFER_ENGINE_OBJS) $(SURFER_MEDIA_OBJS) libvpx_icda.a libopus_icda.a $(SURFER_NET_OBJS) gui.o libicda.o libc_core.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-browser.app crt1.o surfer_surfer.o $(SURFER_ENGINE_OBJS) $(SURFER_MEDIA_OBJS) libvpx_icda.a libopus_icda.a $(SURFER_NET_OBJS) gui.o libicda.o libc_core.o $(shell $(CC) -print-libgcc-file-name)
	cp -f /tmp/icda-browser.app userspace/browser.app


settings.o: userspace/settings.c userspace/settings_wifi.h userspace/settings_updates.h userspace/gui.h $(IC_HEADERS) userspace/icda_sys.h userspace/settings_store.h \
           userspace/font.h userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/settings.c -o /tmp/icda-settings.o
	cp -f /tmp/icda-settings.o settings.o

settings_wifi.o: userspace/settings_wifi.c userspace/settings_wifi.h $(IC_HEADERS) userspace/icda_sys.h Makefile
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/settings_wifi.c -o /tmp/icda-settings_wifi.o
	cp -f /tmp/icda-settings_wifi.o settings_wifi.o

settings_updates.o: userspace/settings_updates.c userspace/settings_updates.h $(IC_HEADERS) userspace/icda_sys.h Makefile
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/settings_updates.c -o /tmp/icda-settings_updates.o
	cp -f /tmp/icda-settings_updates.o settings_updates.o

settings_keys.o: userspace/settings_keys.c userspace/settings_keys.h userspace/shortcuts.h $(IC_HEADERS) userspace/icda_sys.h Makefile
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/settings_keys.c -o /tmp/icda-settings_keys.o
	cp -f /tmp/icda-settings_keys.o settings_keys.o

userspace/settings.app: crt0.o settings.o settings_wifi.o settings_updates.o settings_keys.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-settings.app crt0.o settings.o settings_wifi.o settings_updates.o settings_keys.o gui.o libicda.o
	cp -f /tmp/icda-settings.app userspace/settings.app

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
	for f in $(AUDIO_WAVS); do \
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

font_assets.o: kernel/proc/font_assets.asm resources/fonts/ui-2x.icf resources/fonts/Inter-Regular.ttf resources/fonts/Inter-SemiBold.ttf resources/fonts/JetBrainsMono-Regular.ttf
	$(ASM) -f elf64 kernel/proc/font_assets.asm -o font_assets.o

curl_start.o: userspace/curl_start.asm
	$(ASM) -f elf64 userspace/curl_start.asm -o /tmp/icda-curl_start.o
	cp -f /tmp/icda-curl_start.o curl_start.o

curl.o: userspace/curl.c userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/curl.c -o /tmp/icda-curl.o
	cp -f /tmp/icda-curl.o curl.o

userspace/curl.app: curl_start.o curl.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-curl.app curl_start.o curl.o
	cp -f /tmp/icda-curl.app userspace/curl.app

init_start.o: userspace/init_start.asm
	$(ASM) -f elf64 userspace/init_start.asm -o /tmp/icda-init_start.o
	cp -f /tmp/icda-init_start.o init_start.o

init.o: userspace/init.c userspace/icda_sys.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/init.c -o /tmp/icda-init.o
	cp -f /tmp/icda-init.o init.o

userspace/init.app: init_start.o init.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-init.app init_start.o init.o
	cp -f /tmp/icda-init.app userspace/init.app

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





libicda_core.o: userspace/libicda.c $(IC_HEADERS) userspace/icon_data.h userspace/font.h \
                userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/libicda.c -o /tmp/icda-libicda_core.o
	cp -f /tmp/icda-libicda_core.o libicda_core.o

$(IC_MODULE_OBJS): ic_%.o: userspace/ic_%.c $(IC_HEADERS)
	$(CC) $(USR_CFLAGS) -Iuserspace -c $< -o /tmp/icda-$@
	cp -f /tmp/icda-$@ $@

libicda.o: libicda_core.o $(IC_MODULE_OBJS)
	ld -r -o /tmp/icda-libicda.o libicda_core.o $(IC_MODULE_OBJS)
	cp -f /tmp/icda-libicda.o libicda.o

gui_demo.o: userspace/gui_demo.c $(IC_HEADERS) userspace/icda_sys.h \
            userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/gui_demo.c -o /tmp/icda-gui_demo.o
	cp -f /tmp/icda-gui_demo.o gui_demo.o

userspace/gui_demo.app: gui_demo.o crt0.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-gui_demo.app gui_demo.o crt0.o gui.o libicda.o
	cp -f /tmp/icda-gui_demo.app userspace/gui_demo.app

wm.o: userspace/wm.c userspace/wm_frame.h userspace/wm_shell.h userspace/wm_wifi.h userspace/shortcuts.h userspace/gui_proto.h $(IC_HEADERS) userspace/icon_data.h userspace/font.h userspace/icda_sys.h \
      userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/wm.c -o /tmp/icda-wm.o
	cp -f /tmp/icda-wm.o wm.o

wm_frame.o: userspace/wm_frame.c userspace/wm_frame.h $(IC_HEADERS)
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/wm_frame.c -o /tmp/icda-wm_frame.o
	cp -f /tmp/icda-wm_frame.o wm_frame.o

wm_shell.o: userspace/wm_shell.c userspace/wm_shell.h userspace/wm_wifi.h $(IC_HEADERS) userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/wm_shell.c -o /tmp/icda-wm_shell.o
	cp -f /tmp/icda-wm_shell.o wm_shell.o

wm_wifi.o: userspace/wm_wifi.c userspace/wm_wifi.h userspace/wm_shell.h $(IC_HEADERS)
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/wm_wifi.c -o /tmp/icda-wm_wifi.o
	cp -f /tmp/icda-wm_wifi.o wm_wifi.o

userspace/wm.app: crt0.o wm.o wm_frame.o wm_shell.o wm_wifi.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-wm.app crt0.o wm.o wm_frame.o wm_shell.o wm_wifi.o gui.o libicda.o
	cp -f /tmp/icda-wm.app userspace/wm.app

desktop.o: userspace/desktop.c userspace/gui.h userspace/gui_proto.h $(IC_HEADERS) userspace/font.h userspace/icda_sys.h \
           userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/desktop.c -o /tmp/icda-desktop.o
	cp -f /tmp/icda-desktop.o desktop.o

userspace/desktop.app: crt0.o desktop.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-desktop.app crt0.o desktop.o gui.o libicda.o
	cp -f /tmp/icda-desktop.app userspace/desktop.app

terminal.o: userspace/terminal.c userspace/gui.h $(IC_HEADERS) userspace/icda_sys.h \
            userspace/ic_version.h version.h
	$(CC) $(USR_CFLAGS) -Iuserspace -c userspace/terminal.c -o /tmp/icda-terminal.o
	cp -f /tmp/icda-terminal.o terminal.o

userspace/terminal.app: crt0.o terminal.o gui.o libicda.o userspace/user.ld
	ld -nostdlib -static -T userspace/user.ld -o /tmp/icda-terminal.app crt0.o terminal.o gui.o libicda.o
	cp -f /tmp/icda-terminal.app userspace/terminal.app




USER_PROGS_PROD = userspace/hello.icx userspace/pid.icx userspace/ticker.icx userspace/hello.elf userspace/pid.elf userspace/argc.elf userspace/libctest.elf userspace/fetch.elf userspace/updated.elf userspace/media.app userspace/editor.app userspace/diskman.app userspace/curl.app userspace/wm.app userspace/desktop.app userspace/terminal.app userspace/taskman.app userspace/browser.app userspace/settings.app userspace/init.app
USER_PROGS_TEST = userspace/gui_demo.app userspace/nptest.app userspace/nptestlx.elf
ifeq ($(CI_IMAGE),1)
USER_PROGS_ALL = $(USER_PROGS_PROD) $(USER_PROGS_TEST)
else
USER_PROGS_ALL = $(USER_PROGS_PROD)
endif

user_programs_slim.o: kernel/proc/user_programs.asm userspace/init.app
	$(ASM) -f elf64 -DCI_IMAGE=0 -DSLIM=1 kernel/proc/user_programs.asm -o user_programs_slim.o

user_programs.o: kernel/proc/user_programs.asm $(USER_PROGS_ALL) resources/linux/busybox resources/ssl/cacert.pem
	$(ASM) -f elf64 -DCI_IMAGE=$(CI_IMAGE) kernel/proc/user_programs.asm -o user_programs.o

kernel/install-kernel.bin: kernel.o device.o speaker.o playback.o hda.o e1000.o virtio_net.o net_drv.o net.o sock.o vga.o framebuffer.o gpu.o virtio_gpu.o flip.o keyboard.o input.o mouse.o shm.o msgq.o devops.o devnodes.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs_install.o font_assets.o install.o diskfmt.o vfs.o fd.o lx.o lx_vm.o persistfs.o bootlog.o sysupdate.o fat32.o fatfs.o exfatfs.o ntfsfs.o volumes.o exfat.o ntfs.o tty.o pty.o syscall.o console.o serial.o gdt.o idt.o isr.o pic.o lapic.o smp.o tsc.o ap_blob.o pat.o fpu.o rtc.o ioapic.o irq_controller.o acpi.o efi.o pmm.o heap.o vmm.o pf.o bootstage.o splash.o power.o vt.o \
            sched.o sched_asm.o user.o user_enter.o user_programs_slim.o shell_blob.o boot.o gdt_flush.o isr_asm.o \
            sha256.o sha1.o aes.o bn.o rsa.o x25519.o gcm.o tls.o $(IWM_OBJS)
	$(CC) -T kernel/linker.ld -o kernel/install-kernel.bin -ffreestanding -O0 -nostdlib \
	      -fno-pie -no-pie boot.o kernel.o device.o speaker.o playback.o hda.o e1000.o virtio_net.o net_drv.o net.o sock.o vga.o framebuffer.o gpu.o virtio_gpu.o flip.o keyboard.o input.o mouse.o shm.o msgq.o devops.o devnodes.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs_install.o font_assets.o install.o diskfmt.o vfs.o fd.o lx.o lx_vm.o persistfs.o bootlog.o sysupdate.o fat32.o fatfs.o exfatfs.o ntfsfs.o volumes.o exfat.o ntfs.o tty.o pty.o syscall.o console.o serial.o power.o vt.o \
	      gdt.o idt.o isr.o pic.o lapic.o smp.o tsc.o ap_blob.o pat.o fpu.o rtc.o ioapic.o irq_controller.o acpi.o efi.o pmm.o heap.o vmm.o pf.o \
	      bootstage.o splash.o sched.o sched_asm.o user.o user_enter.o user_programs_slim.o shell_blob.o gdt_flush.o isr_asm.o \
	      sha256.o sha1.o aes.o bn.o rsa.o x25519.o gcm.o tls.o $(IWM_OBJS) -lgcc

kernel.bin: kernel.o device.o speaker.o playback.o hda.o e1000.o virtio_net.o net_drv.o net.o sock.o vga.o framebuffer.o gpu.o virtio_gpu.o flip.o keyboard.o input.o mouse.o shm.o msgq.o devops.o devnodes.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs.o install.o diskfmt.o audio_assets_gen.o icon_assets_gen.o icon_assets.o font_assets.o vfs.o fd.o lx.o lx_vm.o persistfs.o bootlog.o sysupdate.o fat32.o fatfs.o exfatfs.o ntfsfs.o volumes.o exfat.o ntfs.o tty.o pty.o syscall.o console.o serial.o gdt.o idt.o isr.o pic.o lapic.o smp.o tsc.o ap_blob.o pat.o fpu.o rtc.o ioapic.o irq_controller.o acpi.o efi.o pmm.o heap.o vmm.o pf.o bootstage.o splash.o power.o vt.o \
            sched.o sched_asm.o user.o user_enter.o user_programs.o audio_assets.o shell_blob.o boot_assets.o boot.o gdt_flush.o isr_asm.o \
            sha256.o sha1.o aes.o bn.o rsa.o x25519.o gcm.o tls.o $(IWM_OBJS)
	$(CC) -T kernel/linker.ld -o kernel.bin -ffreestanding -O0 -nostdlib \
	      -fno-pie -no-pie boot.o kernel.o device.o speaker.o playback.o hda.o e1000.o virtio_net.o net_drv.o net.o sock.o vga.o framebuffer.o gpu.o virtio_gpu.o flip.o keyboard.o input.o mouse.o shm.o msgq.o devops.o devnodes.o nvme.o ahci.o ata.o block.o partition.o pci.o initramfs.o install.o diskfmt.o audio_assets_gen.o icon_assets_gen.o icon_assets.o font_assets.o vfs.o fd.o lx.o lx_vm.o persistfs.o bootlog.o sysupdate.o fat32.o fatfs.o exfatfs.o ntfsfs.o volumes.o exfat.o ntfs.o tty.o pty.o syscall.o console.o serial.o power.o vt.o \
	      gdt.o idt.o isr.o pic.o lapic.o smp.o tsc.o ap_blob.o pat.o fpu.o rtc.o ioapic.o irq_controller.o acpi.o efi.o pmm.o heap.o vmm.o pf.o \
	      bootstage.o splash.o sched.o sched_asm.o user.o user_enter.o user_programs.o audio_assets.o shell_blob.o boot_assets.o gdt_flush.o isr_asm.o \
	      sha256.o sha1.o aes.o bn.o rsa.o x25519.o gcm.o tls.o $(IWM_OBJS) -lgcc

kernel.iso: kernel.bin
	mkdir -p isodir/boot/grub
	mkdir -p isodir/EFI/BOOT
	cp kernel.bin isodir/boot/kernel.bin
	cp boot/grub/grub.cfg isodir/boot/grub/grub.cfg
	grub-mkstandalone -O x86_64-efi -o isodir/EFI/BOOT/BOOTX64.EFI "boot/grub/grub.cfg=boot/grub/grub.cfg"
	grub-mkrescue -o /tmp/kernel.iso isodir
	
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











qemu-power: kernel.iso
	$(QEMU) -cdrom kernel.iso -m 256M -serial stdio -display none -monitor none \
		-device isa-debug-exit,iobase=0x501,iosize=0x04

qemu-power-reboot: kernel.iso
	$(QEMU) -cdrom kernel.iso -m 256M -serial stdio -display none -monitor none \
		-device isa-debug-exit,iobase=0x501,iosize=0x04

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




VENTOY_ISO := $(firstword $(wildcard /run/media/*/Ventoy/kernel.iso /media/*/Ventoy/kernel.iso))

usb-sync: kernel.iso
ifeq ($(strip $(VENTOY_ISO)),)
	@echo "usb-sync: no Ventoy USB with kernel.iso present, skipping"
else
	rm -f "$(VENTOY_ISO)"
	cp kernel.iso "$(VENTOY_ISO).tmp"
	mv -f "$(VENTOY_ISO).tmp" "$(VENTOY_ISO)"
	@echo "usb-sync: installed kernel.iso -> $(VENTOY_ISO)"
endif

.PHONY: all clean wifi-crypto-test wifi-sim-test qemu qemu-headless qemu-uefi qemu-uefi-headless qemu-smoke qemu-power qemu-power-reboot docker-image docker-build docker-qemu docker-qemu-headless docker-qemu-uefi docker-qemu-uefi-headless docker-smoke usb-sync sdk

sdk: crt1.o libc.o userspace/user.ld $(LIBC_HEADERS) userspace/libc/sdk/Makefile
	rm -rf sdk
	mkdir -p sdk/include sdk/lib
	cp $(LIBC_HEADERS) userspace/icda_sys.h userspace/ic_mem.h sdk/include/
	cp crt1.o libc.o userspace/user.ld sdk/lib/
	cp userspace/libc/sdk/Makefile userspace/libc/sdk/hello.c userspace/libc/sdk/README.txt sdk/
