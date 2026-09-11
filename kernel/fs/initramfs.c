#include "initramfs.h"
#include "vfs.h"
#include "audio_assets_gen.h"
#include "icon_assets_gen.h"

#include <stdint.h>

#ifndef INITRAMFS_INCLUDE_AUDIO_ASSETS
#define INITRAMFS_INCLUDE_AUDIO_ASSETS 1
#endif

#ifndef INITRAMFS_INCLUDE_ICON_ASSETS
#define INITRAMFS_INCLUDE_ICON_ASSETS 1
#endif

/* CI test-image extras (nptest/nptestlx/gui_demo). Default off:
 * `make` builds the production image; CI builds with CI_IMAGE=1. */
#ifndef CI_IMAGE
#define CI_IMAGE 0
#endif

extern const char userprog_hello_start[];
extern const char userprog_hello_end[];
extern const char userprog_pid_start[];
extern const char userprog_pid_end[];
extern const char userprog_hello_elf_start[];
extern const char userprog_hello_elf_end[];
extern const char userprog_pid_elf_start[];
extern const char userprog_pid_elf_end[];
extern const char userprog_argc_elf_start[];
extern const char userprog_argc_elf_end[];
extern const char userprog_ticker_start[];
extern const char userprog_ticker_end[];
extern const char userprog_audioplay_start[];
extern const char userprog_audioplay_end[];
extern const char userprog_editor_start[];
extern const char userprog_editor_end[];
extern const char userprog_diskman_start[];
extern const char userprog_diskman_end[];
extern const char userprog_curl_start[];
extern const char userprog_curl_end[];
extern const char usershell_start[];
extern const char usershell_end[];
extern const char userprog_wm_start[];
extern const char userprog_wm_end[];
extern const char userprog_desktop_start[];
extern const char userprog_desktop_end[];
extern const char userprog_terminal_start[];
extern const char userprog_terminal_end[];
#if CI_IMAGE
extern const char userprog_gui_demo_start[];
extern const char userprog_gui_demo_end[];
#endif
extern const char userprog_taskman_start[];
extern const char userprog_taskman_end[];
extern const char userprog_browser_start[];
extern const char userprog_browser_end[];
extern const char userprog_settings_start[];
extern const char userprog_settings_end[];
#if CI_IMAGE
extern const char userprog_nptest_start[];
extern const char userprog_nptest_end[];
extern const char userprog_nptestlx_start[];
extern const char userprog_nptestlx_end[];
#endif
extern const char userprog_init_start[];
extern const char userprog_init_end[];
static const char motd_txt[] =
    "welcome to icda\n"
    "\n"
    "this system boots into a userspace shell with a persistent writable home area.\n"
    "start in /home and try: touch note, write note hello, cat note, sync.\n";

static const char commands_txt[] =
    "user shell commands:\n"
    "  help           show available commands\n"
    "  clear          clear the screen\n"
    "  pwd            print current directory\n"
    "  cd <path>      change current directory\n"
    "  ls [path]      list files and directories\n"
    "  cat <path>     print a file\n"
    "  echo <text>    print a line of text\n"
    "  mkdir <path>   create a directory\n"
    "  touch <path>   create an empty file\n"
    "  write <path> <text>  replace file contents\n"
    "  stat <path>    show file metadata\n"
    "  install        persist the seeded system into writable disk state\n"
    "  run <path> [args]  launch a user program (.app, .elf, or supported script)\n"
    "  storage        list block devices, partitions, and mounts\n"
    "  mount <n> <path>  mount a detected fat32/exfat partition at a directory\n"
    "  diskman        open the disk manager / formatter\n"
    "  curl <url> <path>  download over plain http into a file (IPv4 URLs only)\n"
    "  sync           flush the writable filesystem to disk\n"
    "  play <path>    play a wav file in the background\n"
    "  stop           stop the current song\n"
    "  edit <path>    open the text editor\n";

static const char roadmap_txt[] =
    "things to try:\n"
    "  - keep notes and files in /home (writable; run sync to persist)\n"
    "  - explore /apps for the installed programs\n"
    "  - read /usr/share/commands.txt for shell commands\n"
    "  - open the desktop, terminal, editor, and browser from the menu\n";

static const char files_txt[] =
    "mounted roots:\n"
    "  /etc      basic system text\n"
    "  /apps     native app-facing launch paths\n"
    "  /bin      tiny user programs\n"
    "  /usr/share terminal docs\n"
    "  /home     default writable user area\n"
    "  /volumes  detected filesystem mounts\n";

static initramfs_file_t initramfs_files[] = {
    { "/etc/motd.txt", motd_txt, sizeof(motd_txt) - 1 },
    { "/usr/share/commands.txt", commands_txt, sizeof(commands_txt) - 1 },
    { "/usr/share/roadmap.txt", roadmap_txt, sizeof(roadmap_txt) - 1 },
    { "/etc/files.txt", files_txt, sizeof(files_txt) - 1 },
    { "/apps/hello.app", 0, 0 },
    { "/apps/pid.app", 0, 0 },
    { "/apps/ticker.app", 0, 0 },
    { "/apps/shell.app", 0, 0 },
    { "/apps/audioplay.app", 0, 0 },
    { "/apps/editor.app", 0, 0 },
    { "/apps/diskman.app", 0, 0 },
    { "/apps/curl.app", 0, 0 },
    { "/bin/hello.elf", 0, 0 },
    { "/bin/pid.elf", 0, 0 },
    { "/bin/argc.elf", 0, 0 },
    { "/apps/wm.app", 0, 0 },
    { "/apps/desktop.app", 0, 0 },
    { "/apps/terminal.app", 0, 0 },
#if CI_IMAGE
    { "/apps/gui_demo.app", 0, 0 },
#endif
    { "/apps/taskman.app", 0, 0 },
    { "/apps/browser.app", 0, 0 },
    { "/apps/settings.app", 0, 0 },
#if CI_IMAGE
    { "/apps/nptest.app", 0, 0 },
    { "/bin/nptestlx.elf", 0, 0 },
#endif
    { "/sbin/init.app", 0, 0 }
};

/* Seed one blob entry and advance the cursor. Keeps initramfs_init
 * correct when CI_IMAGE entries are compiled out. */
static void initramfs_seed_at(uint64_t *cursor, const char *start, const char *end) {
    uint64_t n;

    if (!cursor || !start || !end || end < start) {
        return;
    }
    n = *cursor;
    if (n >= sizeof(initramfs_files) / sizeof(initramfs_files[0])) {
        return;
    }
    initramfs_files[n].data = start;
    initramfs_files[n].size = (uint64_t)(end - start);
    *cursor = n + 1;
}

int initramfs_init(void) {
    /* Cursor walks the table in order, so gated-out entries cannot
     * desynchronize the blob assignments. Production entries first,
     * then the CI-only test apps in table order. */
    uint64_t n = 4;

    initramfs_seed_at(&n, userprog_hello_start, userprog_hello_end);
    initramfs_seed_at(&n, userprog_pid_start, userprog_pid_end);
    initramfs_seed_at(&n, userprog_ticker_start, userprog_ticker_end);
    initramfs_seed_at(&n, usershell_start, usershell_end);
    initramfs_seed_at(&n, userprog_audioplay_start, userprog_audioplay_end);
    initramfs_seed_at(&n, userprog_editor_start, userprog_editor_end);
    initramfs_seed_at(&n, userprog_diskman_start, userprog_diskman_end);
    initramfs_seed_at(&n, userprog_curl_start, userprog_curl_end);
    initramfs_seed_at(&n, userprog_hello_elf_start, userprog_hello_elf_end);
    initramfs_seed_at(&n, userprog_pid_elf_start, userprog_pid_elf_end);
    initramfs_seed_at(&n, userprog_argc_elf_start, userprog_argc_elf_end);
    initramfs_seed_at(&n, userprog_wm_start, userprog_wm_end);
    initramfs_seed_at(&n, userprog_desktop_start, userprog_desktop_end);
    initramfs_seed_at(&n, userprog_terminal_start, userprog_terminal_end);
#if CI_IMAGE
    initramfs_seed_at(&n, userprog_gui_demo_start, userprog_gui_demo_end);
#endif
    initramfs_seed_at(&n, userprog_taskman_start, userprog_taskman_end);
    initramfs_seed_at(&n, userprog_browser_start, userprog_browser_end);
    initramfs_seed_at(&n, userprog_settings_start, userprog_settings_end);
#if CI_IMAGE
    initramfs_seed_at(&n, userprog_nptest_start, userprog_nptest_end);
    initramfs_seed_at(&n, userprog_nptestlx_start, userprog_nptestlx_end);
#endif
    initramfs_seed_at(&n, userprog_init_start, userprog_init_end);
    return 0;
}

int initramfs_populate(void) {
    for (uint64_t i = 0; i < sizeof(initramfs_files) / sizeof(initramfs_files[0]); i++) {
        if (vfs_seed_readonly(initramfs_files[i].path, initramfs_files[i].data, initramfs_files[i].size) != 0) {
            return -1;
        }
    }
#if INITRAMFS_INCLUDE_AUDIO_ASSETS
    for (uint64_t i = 0; i < generated_audio_asset_count; i++) {
        uint64_t size = (uint64_t)(generated_audio_assets[i].data_end - generated_audio_assets[i].data);
        if (vfs_seed_readonly(generated_audio_assets[i].path, generated_audio_assets[i].data, size) != 0) {
            return -1;
        }
    }
#endif
#if INITRAMFS_INCLUDE_ICON_ASSETS
    for (uint64_t i = 0; i < generated_icon_asset_count; i++) {
        uint64_t size = (uint64_t)(generated_icon_assets[i].data_end - generated_icon_assets[i].data);
        if (vfs_seed_readonly(generated_icon_assets[i].path, generated_icon_assets[i].data, size) != 0) {
            return -1;
        }
    }
#endif
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

#if INITRAMFS_INCLUDE_AUDIO_ASSETS
    for (uint64_t i = 0; i < generated_audio_asset_count; i++) {
        const char *a = generated_audio_assets[i].path;
        const char *b = path;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }

        if (*a == '\0' && *b == '\0') {
            static initramfs_file_t temp;
            temp.path = generated_audio_assets[i].path;
            temp.data = generated_audio_assets[i].data;
            temp.size = (uint64_t)(generated_audio_assets[i].data_end - generated_audio_assets[i].data);
            return &temp;
        }
    }
#endif
#if INITRAMFS_INCLUDE_ICON_ASSETS
    for (uint64_t i = 0; i < generated_icon_asset_count; i++) {
        const char *a = generated_icon_assets[i].path;
        const char *b = path;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }

        if (*a == '\0' && *b == '\0') {
            static initramfs_file_t temp;
            temp.path = generated_icon_assets[i].path;
            temp.data = generated_icon_assets[i].data;
            temp.size = (uint64_t)(generated_icon_assets[i].data_end - generated_icon_assets[i].data);
            return &temp;
        }
    }
#endif

    return 0;
}

const initramfs_file_t *initramfs_file_at(uint64_t index) {
    if (index < sizeof(initramfs_files) / sizeof(initramfs_files[0])) {
        return &initramfs_files[index];
    }
    index -= sizeof(initramfs_files) / sizeof(initramfs_files[0]);
#if INITRAMFS_INCLUDE_AUDIO_ASSETS
    if (index < generated_audio_asset_count) {
        static initramfs_file_t temp;
        temp.path = generated_audio_assets[index].path;
        temp.data = generated_audio_assets[index].data;
        temp.size = (uint64_t)(generated_audio_assets[index].data_end - generated_audio_assets[index].data);
        return &temp;
    }
    index -= generated_audio_asset_count;
#endif
#if INITRAMFS_INCLUDE_ICON_ASSETS
    if (index < generated_icon_asset_count) {
        static initramfs_file_t temp;
        temp.path = generated_icon_assets[index].path;
        temp.data = generated_icon_assets[index].data;
        temp.size = (uint64_t)(generated_icon_assets[index].data_end - generated_icon_assets[index].data);
        return &temp;
    }
#endif
    return 0;
}

uint64_t initramfs_file_count(void) {
    uint64_t count = sizeof(initramfs_files) / sizeof(initramfs_files[0]);
#if INITRAMFS_INCLUDE_AUDIO_ASSETS
    count += generated_audio_asset_count;
#endif
#if INITRAMFS_INCLUDE_ICON_ASSETS
    count += generated_icon_asset_count;
#endif
    return count;
}

uint64_t initramfs_total_bytes(void) {
    uint64_t total = 0;

    for (uint64_t i = 0; i < sizeof(initramfs_files) / sizeof(initramfs_files[0]); i++) {
        total += initramfs_files[i].size;
    }
 #if INITRAMFS_INCLUDE_AUDIO_ASSETS
    for (uint64_t i = 0; i < generated_audio_asset_count; i++) {
        total += (uint64_t)(generated_audio_assets[i].data_end - generated_audio_assets[i].data);
    }
 #endif
 #if INITRAMFS_INCLUDE_ICON_ASSETS
    for (uint64_t i = 0; i < generated_icon_asset_count; i++) {
        total += (uint64_t)(generated_icon_assets[i].data_end - generated_icon_assets[i].data);
    }
 #endif

    return total;
}
