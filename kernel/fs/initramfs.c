#include "initramfs.h"
#include "vfs.h"

#include <stdint.h>

extern const char userprog_hello_start[];
extern const char userprog_hello_end[];
extern const char userprog_pid_start[];
extern const char userprog_pid_end[];
extern const char userprog_hello_elf_start[];
extern const char userprog_hello_elf_end[];
extern const char userprog_pid_elf_start[];
extern const char userprog_pid_elf_end[];
extern const char userprog_ticker_start[];
extern const char userprog_ticker_end[];
extern const char usershell_start[];
extern const char usershell_end[];
extern const char asset_hava_wav_start[];
extern const char asset_hava_wav_end[];

static const char motd_txt[] =
    "welcome to icda\n"
    "\n"
    "this system now boots with a tiny initramfs plus a persistent writable disk layer.\n"
    "try: ls, mkdir home, cd home, touch note, write note hello, cat note, reboot and check again.\n";

static const char commands_txt[] =
    "built-ins:\n"
    "  help           show available commands\n"
    "  clear          clear the screen\n"
    "  mem            show memory usage\n"
    "  ls [prefix]    list initramfs files\n"
    "  cat <path>     print a file from initramfs\n"
    "  run <path>     launch a user program (.app, .elf, or other supported format)\n"
    "  sync           flush the writable filesystem to disk\n"
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
    "  /apps     native app-facing launch paths\n"
    "  /bin      tiny user programs\n"
    "  /usr/share terminal docs\n"
    "  /home     recommended writable user area\n";

static initramfs_file_t initramfs_files[] = {
    { "/etc/motd.txt", motd_txt, sizeof(motd_txt) - 1 },
    { "/usr/share/commands.txt", commands_txt, sizeof(commands_txt) - 1 },
    { "/usr/share/roadmap.txt", roadmap_txt, sizeof(roadmap_txt) - 1 },
    { "/etc/files.txt", files_txt, sizeof(files_txt) - 1 },
    { "/apps/hello.app", 0, 0 },
    { "/apps/pid.app", 0, 0 },
    { "/apps/ticker.app", 0, 0 },
    { "/apps/shell.app", 0, 0 },
    { "/bin/hello.elf", 0, 0 },
    { "/bin/pid.elf", 0, 0 },
    { "/usr/share/audio/hava.wav", 0, 0 }
};

int initramfs_init(void) {
    initramfs_files[4].data = userprog_hello_start;
    initramfs_files[4].size = (uint64_t)(userprog_hello_end - userprog_hello_start);
    initramfs_files[5].data = userprog_pid_start;
    initramfs_files[5].size = (uint64_t)(userprog_pid_end - userprog_pid_start);
    initramfs_files[6].data = userprog_ticker_start;
    initramfs_files[6].size = (uint64_t)(userprog_ticker_end - userprog_ticker_start);
    initramfs_files[7].data = usershell_start;
    initramfs_files[7].size = (uint64_t)(usershell_end - usershell_start);
    initramfs_files[8].data = userprog_hello_elf_start;
    initramfs_files[8].size = (uint64_t)(userprog_hello_elf_end - userprog_hello_elf_start);
    initramfs_files[9].data = userprog_pid_elf_start;
    initramfs_files[9].size = (uint64_t)(userprog_pid_elf_end - userprog_pid_elf_start);
    initramfs_files[10].data = asset_hava_wav_start;
    initramfs_files[10].size = (uint64_t)(asset_hava_wav_end - asset_hava_wav_start);
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
