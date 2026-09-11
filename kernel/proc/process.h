#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "../memory/vmm.h"

struct vfs_node;

#define KERNEL_STACK_PAGES  2
#define KERNEL_STACK_SIZE   (KERNEL_STACK_PAGES * PAGE_SIZE_4K)

typedef enum {
    PROCESS_KERNEL = 0,
    PROCESS_USER   = 1
} process_kind_t;

typedef enum {
    PROCESS_NEW = 0,
    PROCESS_READY = 1,
    PROCESS_RUNNING = 2,
    PROCESS_BLOCKED = 3,
    PROCESS_STOPPED = 4,
    PROCESS_EXITED = 5,
    PROCESS_REAPED = 6
} process_state_t;

typedef enum {
    THREAD_READY   = 0,
    THREAD_RUNNING = 1,
    THREAD_BLOCKED = 2,
    THREAD_STOPPED = 3,
    THREAD_ZOMBIE  = 4
} thread_state_t;

typedef enum {
    THREAD_BLOCK_NONE = 0,
    THREAD_BLOCK_SLEEP = 1,
    THREAD_BLOCK_WAIT_CHILD = 2,
    THREAD_BLOCK_INPUT = 3
} thread_block_reason_t;

struct thread;

typedef struct process {
    uint64_t         pid;
    process_kind_t   kind;
    process_state_t  state;
    uint64_t         exit_code;
    uint64_t         session_id;
    uint64_t         process_group_id;
    addr_space_t    *addr_space;
    struct process  *parent;
    struct thread   *main_thread;
    struct vfs_node *cwd;
    struct process  *next_all;
    int linux_personality;
    uint64_t linux_brk_pos;
    uint64_t linux_mmap_next;
    /* External identity (P0 OS-ification, step 2). INERT for now: filled
     * in at spawn, logged at privileged gates, never enforced. Children
     * inherit the parent's values; /sbin/init.app is re-rooted to
     * uid 0 / session leader inside user_spawn_pathv_depth (before the
     * child goes READY, so no gate can observe a half-set identity). */
    uint32_t ex_uid;
    uint64_t ex_token;
    int      ex_session_leader;
    /* Real file-descriptor table for the Linux personality (B2).
     * fds[0..2] are always the console stdio trio and never stored;
     * entries 3..FD_TABLE_SIZE-1 hold VFS nodes when used.  The whole
     * struct comes from zeroed pages (alloc_object_page), so a fresh
     * process starts with fds_inited == 0 and an empty table; the fd
     * helpers below lazily reserve stdio on first use. */
#define FD_TABLE_SIZE 64
    struct vfs_node *fd_nodes[FD_TABLE_SIZE];
    uint64_t fd_off[FD_TABLE_SIZE];
    uint64_t fd_flags[FD_TABLE_SIZE];
    uint8_t  fd_used[FD_TABLE_SIZE];
    int      fds_inited;
    /* Task-manager accounting: scheduler ticks consumed and user memory */
    uint64_t         cpu_ticks;
    uint64_t         mem_bytes;
    char             name[64];
} process_t;

typedef struct thread {
    uint64_t         kernel_rsp;
    uint64_t         tid;
    uint64_t         state;
    process_t       *owner;
    uint64_t         kernel_stack_top;
    void           (*entry)(void);
    struct thread   *next;
    uint64_t         user_rip;
    uint64_t         user_rsp;
    uint64_t         user_return_rsp;
    uint64_t         user_return_rbx;
    uint64_t         user_return_rbp;
    uint64_t         user_return_r12;
    uint64_t         user_return_r13;
    uint64_t         user_return_r14;
    uint64_t         user_return_r15;
    uint64_t         user_return_pending;
    uint64_t         user_entry_stack_top;
    uint64_t         block_reason;
    uint64_t         wake_tick;
} thread_t;

#endif
