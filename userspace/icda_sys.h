#ifndef USERSPACE_ICDA_SYS_H
#define USERSPACE_ICDA_SYS_H

#include <stdint.h>

enum {
    SYS_CONSOLE_WRITE = 0,
    SYS_GET_PID       = 1,
    SYS_VFS_READ      = 2,
    SYS_VFS_WRITE     = 3,
    SYS_EXIT          = 4,
    SYS_INPUT_READ    = 5,
    SYS_GETCWD        = 6,
    SYS_CHDIR         = 7,
    SYS_LIST_DIR      = 8,
    SYS_EXEC          = 9,
    SYS_CONSOLE_CLEAR = 10,
    SYS_CONSOLE_BACKSPACE = 11,
    SYS_MKDIR         = 12,
    SYS_CREATE        = 13,
    SYS_STAT          = 14,
    SYS_LIST_PROCS    = 15,
    SYS_SPAWN         = 16,
    SYS_WAITPID       = 17,
    SYS_YIELD         = 18,
    SYS_SLEEP         = 19,
    SYS_PROC_INFO     = 20,
    SYS_KILL          = 21,
    SYS_SUSPEND       = 22,
    SYS_RESUME        = 23,
    SYS_INPUT_READLINE = 24,
    SYS_SYNC          = 25,
    SYS_CONSOLE_SETCURSOR = 26,
    SYS_STORAGE_INFO  = 27,
    SYS_SOUND_PLAY    = 28,
    SYS_AUDIO_PCM_PLAY = 29,
    SYS_AUDIO_PLAY_FILE = 30,
    SYS_AUDIO_STOP = 31,
    SYS_AUDIO_STATUS = 32,
    SYS_AUDIO_CLAIM = 33,
    SYS_AUDIO_READ_CHUNK = 34,
    SYS_AUDIO_FINISH = 35,
    SYS_TICKS = 36,
    SYS_INPUT_READ_TIMEOUT = 37,
    SYS_VFS_READ_AT = 38,
    SYS_INSTALL_SYSTEM = 39,
    SYS_MOUNT = 40,
    SYS_FORMAT_DEVICE = 41,
    SYS_CONSOLE_SIZE = 42,
    SYS_INSTALL_DEVICE = 43,
    SYS_RUNTIME_DEVICE = 44,
    SYS_INSTALL_PARTITIONS = 45,
    SYS_FORMAT_PARTITION = 46,
    SYS_SET_PARTITION_ROLE = 47,
    SYS_HTTP_GET_IPV4 = 48,
    SYS_CONSOLE_GETCURSOR = 49,
    SYS_DNS_RESOLVE = 50,
    SYS_HTTPS_GET_IPV4 = 51,
    SYS_EXEC_ARGS = 52,
    SYS_SPAWN_ARGS = 53
};

typedef struct {
    uint64_t inode;
    uint64_t size;
    uint64_t created;
    uint64_t modified;
    uint8_t type;
    uint8_t readonly;
} icda_stat_t;

typedef struct {
    uint64_t pid;
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgid;
    uint64_t kind;
    uint64_t state;
    uint64_t exit_code;
} icda_proc_info_t;

typedef struct {
    uint64_t active;
    uint64_t seconds_left;
    uint64_t total_seconds;
    char name[64];
} icda_audio_info_t;

typedef struct {
    uint64_t efi_partition;
    uint64_t root_partition;
    int64_t swap_partition;
} icda_install_plan_t;

static inline uint64_t sys_call0(uint64_t n) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_call1(uint64_t n, uint64_t a0) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_call2(uint64_t n, uint64_t a0, uint64_t a1) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_call3(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2) {
    uint64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1), "d"(a2) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_call4(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_call5(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t sys_call6(uint64_t n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t ret;
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    register uint64_t r9 __asm__("r9") = a5;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}

static inline uint64_t icda_write(const char *text) { return sys_call1(SYS_CONSOLE_WRITE, (uint64_t)(uintptr_t)text); }
static inline uint64_t icda_get_pid(void) { return sys_call0(SYS_GET_PID); }
static inline uint64_t icda_read_file(const char *path, char *buf, uint64_t cap) { return sys_call3(SYS_VFS_READ, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_read_file_at(const char *path, uint64_t offset, char *buf, uint64_t cap) { return sys_call4(SYS_VFS_READ_AT, (uint64_t)(uintptr_t)path, offset, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_write_file(const char *path, const char *buf, uint64_t size) { return sys_call3(SYS_VFS_WRITE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, size); }
static inline long icda_read_char(void) { return (long)sys_call0(SYS_INPUT_READ); }
static inline uint64_t icda_getcwd(char *buf, uint64_t cap) { return sys_call2(SYS_GETCWD, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_chdir(const char *path) { return sys_call1(SYS_CHDIR, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_list_dir(const char *path, char *buf, uint64_t cap) { return sys_call3(SYS_LIST_DIR, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_exec(const char *path) { return sys_call1(SYS_EXEC, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_exec_args(const char *path, const char *args) { return sys_call2(SYS_EXEC_ARGS, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)args); }
static inline void icda_clear(void) { (void)sys_call0(SYS_CONSOLE_CLEAR); }
static inline void icda_backspace(void) { (void)sys_call0(SYS_CONSOLE_BACKSPACE); }
static inline uint64_t icda_mkdir(const char *path) { return sys_call1(SYS_MKDIR, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_create(const char *path) { return sys_call1(SYS_CREATE, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_stat(const char *path, icda_stat_t *out) { return sys_call2(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)out); }
static inline uint64_t icda_list_procs(char *buf, uint64_t cap) { return sys_call2(SYS_LIST_PROCS, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_spawn(const char *path) { return sys_call1(SYS_SPAWN, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_spawn_args(const char *path, const char *args) { return sys_call2(SYS_SPAWN_ARGS, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)args); }
static inline uint64_t icda_waitpid(uint64_t pid) { return sys_call1(SYS_WAITPID, pid); }
static inline void icda_yield(void) { (void)sys_call0(SYS_YIELD); }
static inline void icda_sleep(uint64_t ticks) { (void)sys_call1(SYS_SLEEP, ticks); }
static inline uint64_t icda_proc_info(uint64_t pid, icda_proc_info_t *out) { return sys_call2(SYS_PROC_INFO, pid, (uint64_t)(uintptr_t)out); }
static inline uint64_t icda_kill(uint64_t pid, uint64_t exit_code) { return sys_call2(SYS_KILL, pid, exit_code); }
static inline uint64_t icda_suspend(uint64_t pid) { return sys_call1(SYS_SUSPEND, pid); }
static inline uint64_t icda_resume(uint64_t pid) { return sys_call1(SYS_RESUME, pid); }
static inline uint64_t icda_read_line(char *buf, uint64_t cap) { return sys_call2(SYS_INPUT_READLINE, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_sync(void) { return sys_call0(SYS_SYNC); }
static inline void icda_set_cursor(uint64_t x, uint64_t y) { (void)sys_call2(SYS_CONSOLE_SETCURSOR, x, y); }
static inline uint64_t icda_storage_info(char *buf, uint64_t cap) { return sys_call2(SYS_STORAGE_INFO, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_play_tone(uint64_t frequency_hz, uint64_t ticks) { return sys_call2(SYS_SOUND_PLAY, frequency_hz, ticks); }
static inline uint64_t icda_play_pcm_u8(const uint8_t *buf, uint64_t size, uint64_t sample_rate) { return sys_call3(SYS_AUDIO_PCM_PLAY, (uint64_t)(uintptr_t)buf, size, sample_rate); }
static inline uint64_t icda_play_audio_file(const char *path) { return sys_call1(SYS_AUDIO_PLAY_FILE, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_stop_audio(void) { return sys_call0(SYS_AUDIO_STOP); }
static inline uint64_t icda_audio_info(icda_audio_info_t *out) { return sys_call1(SYS_AUDIO_STATUS, (uint64_t)(uintptr_t)out); }
static inline uint64_t icda_audio_claim(uint64_t *token_out, uint64_t *sample_rate_out) { return sys_call2(SYS_AUDIO_CLAIM, (uint64_t)(uintptr_t)token_out, (uint64_t)(uintptr_t)sample_rate_out); }
static inline uint64_t icda_audio_read_chunk(uint64_t token, uint8_t *buf, uint64_t cap) { return sys_call3(SYS_AUDIO_READ_CHUNK, token, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_audio_finish(uint64_t token) { return sys_call1(SYS_AUDIO_FINISH, token); }
static inline uint64_t icda_ticks(void) { return sys_call0(SYS_TICKS); }
static inline long icda_read_char_timeout(uint64_t ticks) { return (long)sys_call1(SYS_INPUT_READ_TIMEOUT, ticks); }
static inline uint64_t icda_install_system(uint64_t *files_out, uint64_t *bytes_out) { return sys_call2(SYS_INSTALL_SYSTEM, (uint64_t)(uintptr_t)files_out, (uint64_t)(uintptr_t)bytes_out); }
static inline uint64_t icda_install_device(uint64_t device_index, uint64_t *files_out, uint64_t *bytes_out) { return sys_call3(SYS_INSTALL_DEVICE, device_index, (uint64_t)(uintptr_t)files_out, (uint64_t)(uintptr_t)bytes_out); }
static inline uint64_t icda_install_partitions(const icda_install_plan_t *plan, uint64_t *files_out, uint64_t *bytes_out) { return sys_call3(SYS_INSTALL_PARTITIONS, (uint64_t)(uintptr_t)plan, (uint64_t)(uintptr_t)files_out, (uint64_t)(uintptr_t)bytes_out); }
static inline uint64_t icda_mount(uint64_t partition_index, const char *path) { return sys_call2(SYS_MOUNT, partition_index, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_format_device(uint64_t device_index, uint64_t fs_type) { return sys_call2(SYS_FORMAT_DEVICE, device_index, fs_type); }
static inline uint64_t icda_format_partition(uint64_t partition_index, uint64_t fs_type) { return sys_call2(SYS_FORMAT_PARTITION, partition_index, fs_type); }
static inline uint64_t icda_set_partition_role(uint64_t partition_index, uint64_t role) { return sys_call2(SYS_SET_PARTITION_ROLE, partition_index, role); }
static inline uint64_t icda_console_size(uint64_t *cols_out, uint64_t *rows_out) { return sys_call2(SYS_CONSOLE_SIZE, (uint64_t)(uintptr_t)cols_out, (uint64_t)(uintptr_t)rows_out); }
static inline uint64_t icda_console_cursor(uint64_t *x_out, uint64_t *y_out) { return sys_call2(SYS_CONSOLE_GETCURSOR, (uint64_t)(uintptr_t)x_out, (uint64_t)(uintptr_t)y_out); }
static inline uint64_t icda_runtime_device(void) { return sys_call0(SYS_RUNTIME_DEVICE); }
static inline uint64_t icda_http_get_ipv4(uint32_t ipv4_addr, uint64_t port, const char *host, const char *path, const char *out_path, uint64_t *bytes_out) { return sys_call6(SYS_HTTP_GET_IPV4, ipv4_addr, port, (uint64_t)(uintptr_t)host, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)out_path, (uint64_t)(uintptr_t)bytes_out); }
static inline uint64_t icda_https_get_ipv4(uint32_t ipv4_addr, uint64_t port, const char *host, const char *path, const char *out_path, uint64_t *bytes_out) { return sys_call6(SYS_HTTPS_GET_IPV4, ipv4_addr, port, (uint64_t)(uintptr_t)host, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)out_path, (uint64_t)(uintptr_t)bytes_out); }
static inline uint64_t icda_dns_resolve(const char *host, uint32_t *ipv4_out) { return sys_call2(SYS_DNS_RESOLVE, (uint64_t)(uintptr_t)host, (uint64_t)(uintptr_t)ipv4_out); }
static inline void icda_exit(uint64_t code) { (void)sys_call1(SYS_EXIT, code); for (;;) {} }

#endif
