#include "initramfs.h"
#include "vfs.h"

#include <stdint.h>

static const char motd_txt[] =
    "welcome to icda\n"
    "\n"
    "this terminal is still kernel-side, but it now has a tiny initramfs.\n"
    "try: ls, cat /etc/motd.txt, cat /usr/share/commands.txt, mem, reboot\n";

static const char commands_txt[] =
    "built-ins:\n"
    "  help           show available commands\n"
    "  clear          clear the screen\n"
    "  mem            show memory usage\n"
    "  ls [prefix]    list initramfs files\n"
    "  cat <path>     print a file from initramfs\n"
    "  echo <text>    print text\n"
    "  reboot         restart the machine\n";

static const char roadmap_txt[] =
    "roadmap:\n"
    "  1. initramfs-backed shell path\n"
    "  2. process + address-space split\n"
    "  3. syscalls and user mode\n"
    "  4. program loading\n"
    "  5. richer subsystems for hybrid runtime goals\n";

static const char files_txt[] =
    "mounted roots:\n"
    "  /etc      basic system text\n"
    "  /usr/share terminal docs\n"
    "  /var/log  reserved for later runtime logs\n";

static const initramfs_file_t initramfs_files[] = {
    { "/etc/motd.txt", motd_txt, sizeof(motd_txt) - 1 },
    { "/usr/share/commands.txt", commands_txt, sizeof(commands_txt) - 1 },
    { "/usr/share/roadmap.txt", roadmap_txt, sizeof(roadmap_txt) - 1 },
    { "/etc/files.txt", files_txt, sizeof(files_txt) - 1 }
};

int initramfs_init(void) {
    return 0;
}

int initramfs_populate(void) {
    for (uint64_t i = 0; i < sizeof(initramfs_files) / sizeof(initramfs_files[0]); i++) {
        if (vfs_seed_readonly(initramfs_files[i].path, initramfs_files[i].data, initramfs_files[i].size) != 0) {
            return -1;
        }
    }
    return 0;
}

const initramfs_file_t *initramfs_find(const char *path) {
    if (!path) {
        return 0;
    }

    for (uint64_t i = 0; i < sizeof(initramfs_files) / sizeof(initramfs_files[0]); i++) {
        const char *a = initramfs_files[i].path;
        const char *b = path;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }

        if (*a == '\0' && *b == '\0') {
            return &initramfs_files[i];
        }
    }

    return 0;
}

const initramfs_file_t *initramfs_file_at(uint64_t index) {
    if (index >= sizeof(initramfs_files) / sizeof(initramfs_files[0])) {
        return 0;
    }

    return &initramfs_files[index];
}

uint64_t initramfs_file_count(void) {
    return sizeof(initramfs_files) / sizeof(initramfs_files[0]);
}

uint64_t initramfs_total_bytes(void) {
    uint64_t total = 0;

    for (uint64_t i = 0; i < sizeof(initramfs_files) / sizeof(initramfs_files[0]); i++) {
        total += initramfs_files[i].size;
    }

    return total;
}
