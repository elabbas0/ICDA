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
