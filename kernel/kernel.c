#include <stdint.h>

#include "drivers/console/console.h"
#include "drivers/display/framebuffer.h"
#include "drivers/display/vga.h"
#include "drivers/input/input.h"
#include "drivers/input/keyboard.h"
#include "drivers/pci/pci.h"
#include "drivers/serial/serial.h"
#include "drivers/storage/ahci.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/block.h"
#include "drivers/storage/nvme.h"
#include "drivers/storage/partition.h"
#include "diag/bootstage.h"
#include "fs/fat32.h"
#include "fs/initramfs.h"
#include "fs/persistfs.h"
#include "fs/vfs.h"
#include "syscall/syscall.h"
#include "tty/tty.h"

#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/irq_controller.h"
#include "cpu/isr.h"

#include "memory/pf.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"

#include "proc/sched.h"
#include "proc/user.h"

#ifndef SERIAL_SHELL_MIRROR
#define SERIAL_SHELL_MIRROR 0
#endif

static void timer_handler(struct registers *regs) {
    schedule(regs);
}

static void boot_prefix(const char *topic) {
    console_write("[boot] ", CONSOLE_STYLE_MUTED);
    console_write(topic, CONSOLE_STYLE_ACCENT);
    console_write(": ", CONSOLE_STYLE_MUTED);
}

static void boot_line(const char *topic, const char *message) {
    boot_prefix(topic);
    console_write(message, CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);
}

static void boot_line_dec(const char *topic, const char *message, uint64_t value, const char *suffix) {
    boot_prefix(topic);
    console_write(message, CONSOLE_STYLE_INFO);
    console_write_dec64(value, CONSOLE_STYLE_INFO);
    if (suffix) {
        console_write(suffix, CONSOLE_STYLE_INFO);
    }
    console_write("\n", CONSOLE_STYLE_INFO);
}

static void boot_halt(const char *topic, const char *message) {
    boot_prefix(topic);
    console_write(message, CONSOLE_STYLE_ERROR);
    console_write("\n", CONSOLE_STYLE_ERROR);
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void kernel_main(void *multiboot_info) {
    serial_init();
    bootstage_set(1, "serial");

    int has_fb = fb_init(multiboot_info);
    if (!has_fb) {
        vga_init();
    }
    bootstage_set(2, has_fb ? "framebuffer" : "vga");
    console_init(has_fb);

    console_write("ICDA Boot Sequence\n\n", CONSOLE_STYLE_INFO);
    if (has_fb) {
        boot_prefix("display");
        console_write("framebuffer attached ", CONSOLE_STYLE_INFO);
        console_write_dec64((uint64_t)fb_width, CONSOLE_STYLE_INFO);
        console_write("x", CONSOLE_STYLE_MUTED);
        console_write_dec64((uint64_t)fb_height, CONSOLE_STYLE_INFO);
        console_write("\n", CONSOLE_STYLE_INFO);
    } else {
        boot_line("display", "vga fallback attached");
    }

    gdt_init();
    bootstage_set(3, "gdt");
    boot_line("cpu", "gdt loaded");

    idt_init();
    bootstage_set(4, "idt");
    boot_line("cpu", "idt loaded");

    pmm_init(multiboot_info);
    if (pmm_free_frames() == 0) {
        boot_prefix("memory");
        console_write("no free frames discovered\n", CONSOLE_STYLE_ERROR);
        pmm_print_stats();
        boot_halt("memory", "physical memory manager refused to start");
    }
    bootstage_set(5, "pmm");
    boot_prefix("memory");
    console_write("pmm online, free=", CONSOLE_STYLE_INFO);
    console_write_dec64(pmm_free_frames(), CONSOLE_STYLE_INFO);
    console_write(" total=", CONSOLE_STYLE_MUTED);
    console_write_dec64(pmm_total_frames(), CONSOLE_STYLE_INFO);
    console_write(" frames\n", CONSOLE_STYLE_INFO);

    if (vmm_init(fb_phys_addr(), fb_phys_size()) != 0) {
        boot_halt("memory", "virtual memory manager failed to map kernel space");
    }
    bootstage_set(6, "vmm");
    boot_line("memory", "higher-half mappings active");

    if (heap_init() != 0) {
        boot_halt("memory", "kernel heap allocator failed to initialize");
    }
    bootstage_set(7, "heap");
    boot_line_dec("memory", "kernel heap reserved ", heap_bytes_total(), " bytes");

    if (irq_controller_init(multiboot_info) != 0) {
        boot_halt("interrupts", "failed to enable local apic / ioapic");
    }
    bootstage_set(8, "apic");
    boot_prefix("interrupts");
    console_write(irq_controller_name(), CONSOLE_STYLE_INFO);
    console_write(" active\n", CONSOLE_STYLE_INFO);

    pf_init();
    bootstage_set(9, "pf");
    boot_line("interrupts", "page fault handler armed");

    sched_init();
    bootstage_set(10, "sched");
    boot_line("scheduler", "scheduler core online");

    irq_register(0, timer_handler);
    boot_line("timer", "irq0 handler registered");

    keyboard_init();
    bootstage_set(11, "kbd");
    boot_line("input", "ps2 keyboard attached");

    if (pci_init() == 0) {
        bootstage_set(12, "pci");
        boot_prefix("pci");
        console_write("devices discovered=", CONSOLE_STYLE_INFO);
        console_write_dec64(pci_device_count(), CONSOLE_STYLE_INFO);
        console_write("\n", CONSOLE_STYLE_INFO);
    } else {
        boot_line("pci", "no pci devices discovered");
    }

    {
        const pci_device_t *nvme_pci = pci_find_class(0x01, 0x08);
        if (nvme_pci) {
            boot_prefix("storage");
            console_write("nvme controller vendor=", CONSOLE_STYLE_INFO);
            console_write_hex64(nvme_pci->vendor_id, CONSOLE_STYLE_INFO);
            console_write(" device=", CONSOLE_STYLE_MUTED);
            console_write_hex64(nvme_pci->device_id, CONSOLE_STYLE_INFO);
            console_write(" progif=", CONSOLE_STYLE_MUTED);
            console_write_hex64(nvme_pci->prog_if, CONSOLE_STYLE_INFO);
            console_write("\n", CONSOLE_STYLE_INFO);
        }
    }

    if (nvme_init() == 0) {
        bootstage_set(13, "nvme");
        boot_prefix("storage");
        console_write("nvme online, devices=", CONSOLE_STYLE_INFO);
        console_write_dec64(nvme_device_count(), CONSOLE_STYLE_INFO);
        console_write("\n", CONSOLE_STYLE_INFO);
    } else if (ahci_init() == 0) {
        bootstage_set(14, "ahci");
        boot_prefix("storage");
        console_write("ahci online, devices=", CONSOLE_STYLE_INFO);
        console_write_dec64(ahci_device_count(), CONSOLE_STYLE_INFO);
        console_write("\n", CONSOLE_STYLE_INFO);
    } else if (ata_init() == 0) {
        bootstage_set(15, "ata");
        boot_prefix("storage");
        console_write("legacy ata online, devices=1\n", CONSOLE_STYLE_INFO);
    } else {
        boot_line("storage", "no ata/ahci disk detected");
    }

    if (vfs_init() != 0) {
        boot_halt("storage", "virtual filesystem core failed to initialize");
    }
    bootstage_set(16, "vfs");
    if (initramfs_init() != 0) {
        boot_halt("storage", "initramfs image is not valid");
    }
    if (initramfs_populate() != 0) {
        boot_halt("storage", "initramfs could not populate the live filesystem");
    }
    bootstage_set(17, "initramfs");
    (void)vfs_mkdir(vfs_root(), "/home");
    boot_prefix("storage");
    console_write("ramfs online, seeded files=", CONSOLE_STYLE_INFO);
    console_write_dec64(initramfs_file_count(), CONSOLE_STYLE_INFO);
    console_write(" bytes=", CONSOLE_STYLE_MUTED);
    console_write_dec64(initramfs_total_bytes(), CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);

    if (persistfs_init() == 0 && persistfs_present()) {
        bootstage_set(18, "persist");
        boot_prefix("storage");
        console_write("persistent disk online, loaded entries=", CONSOLE_STYLE_INFO);
        console_write_dec64(persistfs_loaded_entries(), CONSOLE_STYLE_INFO);
        console_write("\n", CONSOLE_STYLE_INFO);
    } else {
        boot_line("storage", "persistent disk unavailable, continuing with ramfs only");
    }

    (void)partition_scan_all();
    bootstage_set(19, "partitions");
    boot_prefix("storage");
    console_write("block devices=", CONSOLE_STYLE_INFO);
    console_write_dec64(block_count(), CONSOLE_STYLE_INFO);
    console_write(" partitions=", CONSOLE_STYLE_MUTED);
    console_write_dec64(partition_count(), CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);

    (void)fat32_mount_detected();
    bootstage_set(20, "fat32");
    if (fat32_mount_count() > 0) {
        boot_prefix("storage");
        console_write("fat32 volumes mounted=", CONSOLE_STYLE_INFO);
        console_write_dec64(fat32_mount_count(), CONSOLE_STYLE_INFO);
        console_write(" at /volumes\n", CONSOLE_STYLE_INFO);
    }

    syscall_init();
    bootstage_set(21, "syscall");
    boot_line("syscall", "int 0x80 dispatcher armed");

#if !SERIAL_SHELL_MIRROR
    if (has_fb) {
        console_set_serial_mirror(0);
    }
#endif
    {
        if (has_fb) {
            console_clear();
        }
        bootstage_set(22, "shell");
        int shell_rc = user_run_path("/apps/shell.app");
        if (shell_rc < 0) {
            boot_line("shell", "userspace shell failed to start, entering recovery console");
            if (tty_init() != 0) {
                console_write("\n", CONSOLE_STYLE_INFO);
                boot_halt("tty", "recovery console failed to start");
            }
        } else {
            boot_prefix("shell");
            console_write("userspace shell exited rc=", CONSOLE_STYLE_INFO);
            console_write_dec64((uint64_t)shell_rc, CONSOLE_STYLE_INFO);
            console_write(", entering recovery console\n", CONSOLE_STYLE_INFO);
            if (tty_init() != 0) {
                console_write("\n", CONSOLE_STYLE_INFO);
                boot_halt("tty", "recovery console failed to start");
            }
        }
    }
    while (1) {
        tty_poll();
    }
}
