#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "../memory/vmm.h"

#define KERNEL_STACK_PAGES  2
#define KERNEL_STACK_SIZE   (KERNEL_STACK_PAGES * PAGE_SIZE_4K)

typedef enum {
    PROCESS_KERNEL = 0,
    PROCESS_USER   = 1
} process_kind_t;

typedef enum {
    THREAD_READY   = 0,
    THREAD_RUNNING = 1,
    THREAD_BLOCKED = 2,
    THREAD_ZOMBIE  = 3
} thread_state_t;

struct thread;

typedef struct process {
    uint64_t         pid;
    process_kind_t   kind;
    addr_space_t    *addr_space;
    struct process  *parent;
    struct thread   *main_thread;
} process_t;

typedef struct thread {
    uint64_t         kernel_rsp;
    uint64_t         tid;
    thread_state_t   state;
    process_t       *owner;
    uint64_t         kernel_stack_top;
    void           (*entry)(void);
    struct thread   *next;
} thread_t;

#endif
