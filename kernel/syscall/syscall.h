#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "../cpu/isr.h"

#define SYSCALL_VECTOR 0x80

/*
 * Errno contract for the validated gate (P0 hardening).
 *
 * Legacy convention is preserved: success is 0 (or a positive value),
 * generic failure is (uint64_t)-1.  The gate below adds precise codes
 * for the failures it newly detects; handlers return them as
 * (uint64_t)-U_Exxx so existing userspace `(long)rc < 0` checks keep
 * working.  Do NOT attach new meanings to -1.  (The page-fault killer
 * in pf.c reports -11 for a dead process, deliberately distinct from
 * U_EFAULT below.)
 */
#define U_ENOENT  2   /* no such file or directory */
#define U_EBADF   9   /* bad file descriptor */
#define U_ENOMEM  12  /* out of memory / unmapped range */
#define U_EACCES  13  /* permission denied (incl. TLS refusing to connect) */
#define U_EFAULT  14  /* bad user-space address */
#define U_EINVAL  22  /* invalid argument */

typedef enum {
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
    SYS_SPAWN_ARGS = 53,
    /* IPC / GUI — added for desktop environment */
    SYS_SHM_CREATE        = 54,
    SYS_SHM_MAP           = 55,
    SYS_SHM_UNMAP         = 56,
    SYS_SHM_CLOSE         = 57,
    SYS_MSG_OPEN          = 58,
    SYS_MSG_SEND          = 59,
    SYS_MSG_RECV          = 60,
    SYS_MSG_POLL          = 61,
    SYS_MAP_FRAMEBUFFER   = 62,
    SYS_INPUT_READ_MOUSE  = 63,
    SYS_GUI_AVAILABLE     = 64,
    /* GPU device layer (DRM/KMS-shaped general GPU driver) */
    SYS_GPU_QUERY         = 65,
    SYS_GPU_PRESENT       = 66,
    SYS_GPU_CURSOR        = 67,
    /* Power management */
    SYS_POWER             = 68,
    /* Process stats for the task manager */
    SYS_PROC_STATS        = 69
} syscall_number_t;

typedef struct {
    uint64_t pid;
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgid;
    uint64_t kind;
    uint64_t state;
    uint64_t exit_code;
} syscall_proc_info_t;

typedef struct {
    uint64_t active;
    uint64_t seconds_left;
    uint64_t total_seconds;
    char name[64];
} syscall_audio_info_t;

typedef struct {
    uint64_t efi_partition;
    uint64_t root_partition;
    int64_t swap_partition;
} syscall_install_plan_t;

typedef struct {
    uint64_t virt_addr;  /* userspace VA of mapped framebuffer */
    int32_t  width;
    int32_t  height;
    uint32_t pitch;      /* bytes per row */
    uint32_t bpp;
} syscall_fb_info_t;

typedef struct {
    char     name[32];   /* driver name, e.g. "fbdev" */
    int32_t  width;      /* current mode */
    int32_t  height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t mode_count;
    uint32_t hw_cursor;  /* device has a hardware cursor plane */
    uint32_t present_supported;
    uint32_t flip_active; /* 1 when tear-free page flipping is active */
} syscall_gpu_info_t;

typedef struct {
    uint64_t cpu_ticks;  /* scheduler ticks consumed by this process */
    uint64_t mem_bytes;  /* user-space pages mapped, in bytes */
    char     name[64];   /* executable basename */
} syscall_proc_stats_t;

typedef struct {
    int32_t  abs_x;
    int32_t  abs_y;
    int32_t  dx;
    int32_t  dy;
    uint8_t  buttons;    /* bit0=left, bit1=right, bit2=middle */
} syscall_mouse_event_t;

void syscall_init(void);
uint64_t syscall_dispatch(struct registers *regs);

uint64_t syscall_kernel_write(const char *text);
uint64_t syscall_kernel_get_pid(void);

#endif
