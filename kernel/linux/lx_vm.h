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
int64_t  lxvm_mmap(process_t *p, uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags, struct vfs_node *node, uint64_t off);
int      lxvm_munmap(process_t *p, uint64_t addr, uint64_t len);
int      lxvm_mprotect(process_t *p, uint64_t addr, uint64_t len, uint32_t prot);
int64_t  lxvm_mremap(process_t *p, uint64_t old, uint64_t old_len, uint64_t new_len, uint32_t flags, uint64_t new_addr);
int      lxvm_madvise(process_t *p, uint64_t addr, uint64_t len, int advice);
int      lxvm_mapped(process_t *p, uint64_t addr);
uint64_t lxvm_maps(process_t *p, char *buf, uint64_t cap);

/* 1 if the fault at addr was a page the process may have (it has it now) */
int      lxvm_fault(process_t *p, uint64_t addr, int write);
int      lxvm_fault_current(uint64_t addr, int write);

int      lxvm_fork(process_t *parent, process_t *child);
void     lxvm_free(process_t *p);
int      lxvm_reserve(process_t *p, uint64_t start, uint64_t end, uint32_t prot);
void     lxvm_free_list(void *list);

#endif
