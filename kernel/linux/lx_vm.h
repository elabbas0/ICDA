#ifndef LX_VM_H
#define LX_VM_H

#include <stdint.h>
#include "../proc/process.h"

struct vfs_node;

/* mmap flags as lx_vm takes them */
#define LXVM_SHARED     0x01
#define LXVM_PRIVATE    0x02
#define LXVM_FIXED      0x10
#define LXVM_ANON       0x20
#define LXVM_POPULATE   0x8000
#define LXVM_NOREPLACE  0x100000
#define LXVM_MREMAP_MAYMOVE 1
#define LXVM_MREMAP_FIXED   2

/* results: an address / 0, or minus a Linux errno */
struct lx_file;
/* node: a file read into private pages; file: a memory object (memfd) whose
 * frames a MAP_SHARED mapping uses directly */
int64_t  lxvm_mmap(process_t *p, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, struct vfs_node *node,
                   struct lx_file *file, uint64_t off);
int      lxvm_munmap(process_t *p, uint64_t addr, uint64_t len);
int      lxvm_mprotect(process_t *p, uint64_t addr, uint64_t len, uint32_t prot);
int64_t  lxvm_mremap(process_t *p, uint64_t old, uint64_t old_len, uint64_t new_len, uint32_t flags, uint64_t new_addr);
int      lxvm_madvise(process_t *p, uint64_t addr, uint64_t len, int advice);
int      lxvm_mapped(process_t *p, uint64_t addr);
uint64_t lxvm_maps(process_t *p, char *buf, uint64_t cap);

/* 1 if the fault at addr was a page the process may have (it has it now) */
int      lxvm_fault(process_t *p, uint64_t addr, int write);
int      lxvm_fault_current(uint64_t addr, int write);

/* lets the page allocator take back unmapped cached file pages when short */
void     lxvm_init_reclaim(void);
/* /proc: p's regions in pages (size, resident file and shared pages, private writable size) */
void     lxvm_usage(process_t *p, uint64_t *size, uint64_t *file_resident, uint64_t *data);
/* /proc/meminfo: frames in the file page cache, and those no process maps (reclaimable) */
void     lxvm_cache_usage(uint64_t *cached, uint64_t *reclaimable);
int      lxvm_fork(process_t *parent, process_t *child);
void     lxvm_free(process_t *p);
int      lxvm_reserve(process_t *p, uint64_t start, uint64_t end, uint32_t prot);
void     lxvm_free_list(void *list);
/* region lists were swapped (execve): forget remembered lookups */
void     lxvm_forget_hints(void);
void     lxvm_describe(process_t *p, uint64_t addr, char *out, uint64_t cap);
int      lxvm_is_code(process_t *p, uint64_t addr);
int      lxvm_lookup(process_t *p, uint64_t addr, struct vfs_node **node, uint64_t *off);

/* profiler counters */
typedef struct { uint64_t faults, file_faults, present_faults, vmas, find_cycles, populate_cycles, miss_reads, miss_cycles,
                 pages[3], kind_cycles[3], step_cycles[3], madv_dontneed, madv_free, madv_free_kept; } lxvm_stat_t;   /* by kind: 0 anonymous, 1 file, 2 memory object */
extern lxvm_stat_t lxvm_stat;

/* 1 once if the last page fault failed for want of memory */
int lxvm_take_oom(void);

/* the first shared mapping of p ending after va (swap leaves those alone) */
int lxvm_next_shared(struct process *p, uint64_t va, uint64_t *start, uint64_t *end);

#endif
