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
extern const char userprog_gui_demo_start[];
extern const char userprog_gui_demo_end[];
extern const char userprog_taskman_start[];
extern const char userprog_taskman_end[];
extern const char userprog_browser_start[];
extern const char userprog_browser_end[];
extern const char userprog_nptest_start[];
extern const char userprog_nptest_end[];
extern const char userprog_nptestlx_start[];
extern const char userprog_nptestlx_end[];
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
    "  /home     default writable user area\n"
    "  /volumes  detected filesystem mounts\n";

static const char demo_sh[] =
    "#!/usr/bin/env bash\n"
    "echo script mode online\n"
    "pwd\n"
    "write /home/script-ok done\n";

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
    { "/bin/demo.sh", demo_sh, sizeof(demo_sh) - 1 },
    { "/apps/wm.app", 0, 0 },
    { "/apps/desktop.app", 0, 0 },
    { "/apps/terminal.app", 0, 0 },
    { "/apps/gui_demo.app", 0, 0 },
    { "/apps/taskman.app", 0, 0 },
    { "/apps/browser.app", 0, 0 },
    { "/apps/nptest.app", 0, 0 },
    { "/bin/nptestlx.elf", 0, 0 }
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
    initramfs_files[8].data = userprog_audioplay_start;
    initramfs_files[8].size = (uint64_t)(userprog_audioplay_end - userprog_audioplay_start);
    initramfs_files[9].data = userprog_editor_start;
    initramfs_files[9].size = (uint64_t)(userprog_editor_end - userprog_editor_start);
    initramfs_files[10].data = userprog_diskman_start;
    initramfs_files[10].size = (uint64_t)(userprog_diskman_end - userprog_diskman_start);
    initramfs_files[11].data = userprog_curl_start;
    initramfs_files[11].size = (uint64_t)(userprog_curl_end - userprog_curl_start);
    initramfs_files[12].data = userprog_hello_elf_start;
    initramfs_files[12].size = (uint64_t)(userprog_hello_elf_end - userprog_hello_elf_start);
    initramfs_files[13].data = userprog_pid_elf_start;
    initramfs_files[13].size = (uint64_t)(userprog_pid_elf_end - userprog_pid_elf_start);
    initramfs_files[14].data = userprog_argc_elf_start;
    initramfs_files[14].size = (uint64_t)(userprog_argc_elf_end - userprog_argc_elf_start);
    initramfs_files[16].data = userprog_wm_start;
    initramfs_files[16].size = (uint64_t)(userprog_wm_end - userprog_wm_start);
    initramfs_files[17].data = userprog_desktop_start;
    initramfs_files[17].size = (uint64_t)(userprog_desktop_end - userprog_desktop_start);
    initramfs_files[18].data = userprog_terminal_start;
    initramfs_files[18].size = (uint64_t)(userprog_terminal_end - userprog_terminal_start);
    initramfs_files[19].data = userprog_gui_demo_start;
    initramfs_files[19].size = (uint64_t)(userprog_gui_demo_end - userprog_gui_demo_start);
    initramfs_files[20].data = userprog_taskman_start;
    initramfs_files[20].size = (uint64_t)(userprog_taskman_end - userprog_taskman_start);
    initramfs_files[21].data = userprog_browser_start;
    initramfs_files[21].size = (uint64_t)(userprog_browser_end - userprog_browser_start);
    initramfs_files[22].data = userprog_nptest_start;
    initramfs_files[22].size = (uint64_t)(userprog_nptest_end - userprog_nptest_start);
    initramfs_files[23].data = userprog_nptestlx_start;
    initramfs_files[23].size = (uint64_t)(userprog_nptestlx_end - userprog_nptestlx_start);
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
