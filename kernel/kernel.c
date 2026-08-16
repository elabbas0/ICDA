#include <stdint.h>

#include "drivers/console/console.h"
#include "drivers/audio/speaker.h"
#include "drivers/audio/hda.h"
#include "drivers/audio/playback.h"
#include "drivers/display/framebuffer.h"
#include "drivers/display/vga.h"
#include "drivers/input/input.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/mouse.h"
#include "drivers/pci/pci.h"
#include "drivers/net/e1000.h"
#include "drivers/serial/serial.h"
#include "drivers/storage/ahci.h"
#include "drivers/storage/ata.h"
#include "drivers/storage/block.h"
#include "drivers/storage/nvme.h"
#include "drivers/storage/partition.h"
#include "diag/bootstage.h"
#include "fs/fat32.h"
#include "fs/exfat.h"
#include "fs/initramfs.h"
#include "fs/install.h"
#include "fs/ntfs.h"
#include "fs/persistfs.h"
#include "fs/vfs.h"
#include "net/net.h"
#include "syscall/syscall.h"
#include "tty/tty.h"
#include "vt/vt.h"
#include "cpu/multiboot2.h"

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
#include "net/net.h"

#ifndef SERIAL_SHELL_MIRROR
#define SERIAL_SHELL_MIRROR 0
#endif

#define PIT_BASE_FREQUENCY 1193182U
#define PIT_COMMAND_PORT   0x43
#define PIT_CHANNEL0_PORT  0x40

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void pit_set_frequency(uint32_t hz) {
    uint32_t divisor;

    if (hz == 0) {
        hz = 100;
    }

    divisor = PIT_BASE_FREQUENCY / hz;
    if (divisor == 0) {
        divisor = 1;
    }
    if (divisor > 0xFFFFU) {
        divisor = 0xFFFFU;
    }

    outb(PIT_COMMAND_PORT, 0x36);
    outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFFU));
    outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFFU));
}

static void timer_handler(struct registers *regs) {
    keyboard_pump();
    /* Apply any Ctrl+Alt+F1..F6 virtual terminal switch requested by the
     * keyboard driver (force-exits the foreground user app if needed). */
    vt_tick();
    audio_playback_tick();
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

static int boot_cmdline_has_flag(void *multiboot_info, const char *flag) {
    struct multiboot_info *info = (struct multiboot_info *)multiboot_info;
    uint8_t *tag_ptr;
    uint8_t *end_ptr;
    uint64_t flag_len = 0;

    if (!multiboot_info || !flag || !*flag) return 0;
    while (flag[flag_len]) flag_len++;

    tag_ptr = (uint8_t *)multiboot_info + 8;
    end_ptr = (uint8_t *)multiboot_info + info->total_size;
    while (tag_ptr < end_ptr) {
        struct multiboot_tag *tag = (struct multiboot_tag *)tag_ptr;
        if (tag->type == MULTIBOOT_TAG_TYPE_END) break;
        if (tag->type == MULTIBOOT_TAG_TYPE_CMDLINE) {
            struct multiboot_tag_string *cmd = (struct multiboot_tag_string *)tag;
            char *s = cmd->string;
            uint64_t i = 0;
            while (s[i]) {
                uint64_t j = 0;
                while (flag[j] && s[i + j] == flag[j]) j++;
                if (j == flag_len) {
                    char prev = (i == 0) ? ' ' : s[i - 1];
                    char next = s[i + j];
                    int prev_ok = (prev == ' ' || prev == '\t' || prev == '\n');
                    int next_ok = (next == 0 || next == ' ' || next == '\t' || next == '\n');
                    if (prev_ok && next_ok) return 1;
                }
                i++;
            }
        }
        tag_ptr += (tag->size + (MULTIBOOT_TAG_ALIGN - 1)) & ~(MULTIBOOT_TAG_ALIGN - 1);
    }
    return 0;
}

void kernel_main(void *multiboot_info) {
    int live_installer = boot_cmdline_has_flag(multiboot_info, "icda.live=1");
    serial_init();
    bootstage_set(1, "serial");

    int has_fb = fb_init(multiboot_info);
    if (!has_fb) {
        vga_init();
    }
    bootstage_set(2, has_fb ? "framebuffer" : "vga");
    console_init(has_fb);

    console_write("ICDA Boot Sequence\n\n", CONSOLE_STYLE_INFO);
    if (live_installer) {
        boot_line("mode", "live installer mode");
    }
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
    pit_set_frequency(100);
    irq_controller_unmask(0);

    keyboard_init();
    bootstage_set(11, "kbd");
    boot_line("input", "ps2 keyboard attached");
    irq_register(1, keyboard_irq);
    irq_controller_unmask(1);

    mouse_init();
    irq_register(12, mouse_irq);
    irq_controller_unmask(12);

    if (pci_init() == 0) {
        bootstage_set(12, "pci");
        boot_prefix("pci");
        console_write("devices discovered=", CONSOLE_STYLE_INFO);
        console_write_dec64(pci_device_count(), CONSOLE_STYLE_INFO);
        console_write("\n", CONSOLE_STYLE_INFO);
    } else {
        boot_line("pci", "no pci devices discovered");
    }

    bootstage_set(120, "net");
    if (net_init() == 0) {
        boot_line("network", "intel e1000 online");
    } else {
        boot_prefix("network");
        console_write("intel e1000 unavailable err=", CONSOLE_STYLE_WARN);
        console_write_dec64((uint64_t)net_last_error(), CONSOLE_STYLE_WARN);
        console_write("\n", CONSOLE_STYLE_WARN);
    }

    bootstage_set(121, "hda");
    boot_prefix("audio");
    console_write("probing intel hda\n", CONSOLE_STYLE_MUTED);
    if (hda_init() == 0) {
        boot_line("audio", "intel hda online");
    } else {
        boot_prefix("audio");
        console_write("intel hda unavailable err=", CONSOLE_STYLE_WARN);
        console_write_dec64((uint64_t)hda_last_error(), CONSOLE_STYLE_WARN);
        console_write("\n", CONSOLE_STYLE_WARN);
    }

    bootstage_set(122, "audio");
    speaker_init();
    if (audio_playback_init() == 0) {
        boot_line("audio", "background playback ready");
    } else {
        boot_line("audio", "background playback unavailable");
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

    boot_prefix("storage");
    console_write("probing nvme/ahci/ata\n", CONSOLE_STYLE_MUTED);
    bootstage_set(123, "nvme");
    if (nvme_init() == 0) {
        bootstage_set(13, "nvme");
        boot_prefix("storage");
        console_write("nvme online, devices=", CONSOLE_STYLE_INFO);
        console_write_dec64(nvme_device_count(), CONSOLE_STYLE_INFO);
        console_write("\n", CONSOLE_STYLE_INFO);
    } else {
        bootstage_set(124, "ahci");
        if (ahci_init() == 0) {
            bootstage_set(14, "ahci");
            boot_prefix("storage");
            console_write("ahci online, devices=", CONSOLE_STYLE_INFO);
            console_write_dec64(ahci_device_count(), CONSOLE_STYLE_INFO);
            console_write("\n", CONSOLE_STYLE_INFO);
        } else {
            bootstage_set(125, "ata");
            if (ata_init() == 0) {
                bootstage_set(15, "ata");
                boot_prefix("storage");
                console_write("legacy ata online, devices=", CONSOLE_STYLE_INFO);
                console_write_dec64(ata_device_count(), CONSOLE_STYLE_INFO);
                console_write("\n", CONSOLE_STYLE_INFO);
            } else {
                boot_line("storage", "no ata/ahci disk detected");
            }
        }
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

    (void)partition_scan_all();
    bootstage_set(18, "partitions");
    boot_prefix("storage");
    console_write("block devices=", CONSOLE_STYLE_INFO);
    console_write_dec64(block_count(), CONSOLE_STYLE_INFO);
    console_write(" partitions=", CONSOLE_STYLE_MUTED);
    console_write_dec64(partition_count(), CONSOLE_STYLE_INFO);
    console_write("\n", CONSOLE_STYLE_INFO);

    persistfs_set_live_mode(live_installer);
    if (persistfs_init() == 0 && persistfs_present()) {
        bootstage_set(19, "persist");
        boot_prefix("storage");
        console_write("persistent state online, loaded entries=", CONSOLE_STYLE_INFO);
        console_write_dec64(persistfs_loaded_entries(), CONSOLE_STYLE_INFO);
        if (persistfs_active_partition() >= 0) {
            console_write(" backend=partition", CONSOLE_STYLE_MUTED);
        } else {
            console_write(" backend=device", CONSOLE_STYLE_MUTED);
        }
        console_write("\n", CONSOLE_STYLE_INFO);
        if (system_install_present()) {
            boot_line("system", "installed writable overlay active");
        }
    } else {
        if (live_installer) {
            boot_line("storage", "live mode: disk persistence disabled, continuing with ramfs only");
        } else {
            boot_line("storage", "persistent state unavailable, continuing with ramfs only");
        }
    }

    bootstage_set(20, "mounts");
    boot_line("storage", "automatic volume import deferred; use mount <partition> <path>");

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
        int shell_failures = 0;
        for (;;) {
            /* The active virtual terminal decides what runs: the desktop
             * (WM) on F1, a full-screen text shell on F2+.  A VT switch
             * force-exits the foreground app, which lands us back here to
             * restart with the app for the newly selected VT. */
            int shell_rc = user_run_path(vt_app_path());
            if (shell_rc < 0 && vt_is_gui()) {
                shell_rc = user_run_path("/apps/shell.app");
            }
            if (shell_rc < 0) {
                shell_failures++;
                if (shell_failures < 3) {
                    boot_line("shell", "userspace shell failed to start, retrying");
                    continue;
                }
                boot_line("shell", "userspace shell failed repeatedly, entering recovery console");
                if (tty_init() != 0) {
                    console_write("\n", CONSOLE_STYLE_INFO);
                    boot_halt("tty", "recovery console failed to start");
                }
                break;
            }
            boot_prefix("shell");
            console_write("userspace shell exited rc=", CONSOLE_STYLE_INFO);
            console_write_dec64((uint64_t)shell_rc, CONSOLE_STYLE_INFO);
            console_write(", restarting\n", CONSOLE_STYLE_INFO);
        }
    }
    while (1) {
        tty_poll();
    }
}
