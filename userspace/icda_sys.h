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
    SYS_STAT          = 14
};

typedef struct {
    uint64_t inode;
    uint64_t size;
    uint64_t created;
    uint64_t modified;
    uint8_t type;
    uint8_t readonly;
} icda_stat_t;

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

static inline uint64_t icda_write(const char *text) { return sys_call1(SYS_CONSOLE_WRITE, (uint64_t)(uintptr_t)text); }
static inline uint64_t icda_get_pid(void) { return sys_call0(SYS_GET_PID); }
static inline uint64_t icda_read_file(const char *path, char *buf, uint64_t cap) { return sys_call3(SYS_VFS_READ, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_write_file(const char *path, const char *buf, uint64_t size) { return sys_call3(SYS_VFS_WRITE, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, size); }
static inline long icda_read_char(void) { return (long)sys_call0(SYS_INPUT_READ); }
static inline uint64_t icda_getcwd(char *buf, uint64_t cap) { return sys_call2(SYS_GETCWD, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_chdir(const char *path) { return sys_call1(SYS_CHDIR, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_list_dir(const char *path, char *buf, uint64_t cap) { return sys_call3(SYS_LIST_DIR, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf, cap); }
static inline uint64_t icda_exec(const char *path) { return sys_call1(SYS_EXEC, (uint64_t)(uintptr_t)path); }
static inline void icda_clear(void) { (void)sys_call0(SYS_CONSOLE_CLEAR); }
static inline void icda_backspace(void) { (void)sys_call0(SYS_CONSOLE_BACKSPACE); }
static inline uint64_t icda_mkdir(const char *path) { return sys_call1(SYS_MKDIR, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_create(const char *path) { return sys_call1(SYS_CREATE, (uint64_t)(uintptr_t)path); }
static inline uint64_t icda_stat(const char *path, icda_stat_t *out) { return sys_call2(SYS_STAT, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)out); }
static inline void icda_exit(uint64_t code) { (void)sys_call1(SYS_EXIT, code); for (;;) {} }

#endif
