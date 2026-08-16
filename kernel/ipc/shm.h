#ifndef IPC_SHM_H
#define IPC_SHM_H

#include <stdint.h>

/* Virtual address base for SHM mappings in userspace (each slot 16 MiB).
 * Must live in per-process PML4 space: 0x300000000 sat in PML4[0], which is
 * shared with the kernel identity map, so one process unmapping a region
 * cleared the PTE the other process had mapped. 1 TiB (PML4[2]) is private
 * to each address space, far from both user text (512 GiB) and the stack. */
#define SHM_VIRT_BASE   0x0000010000000000ULL
#define SHM_SLOT_SIZE   (16ULL * 1024ULL * 1024ULL)
#define SHM_MAX_REGIONS 32

/* Returns 1-based handle, 0 on failure */
uint64_t shm_create(uint64_t size);

/* Map handle into current process VA; returns virtual addr or 0 */
uint64_t shm_map(uint64_t handle);

/* Unmap handle from current process */
int shm_unmap(uint64_t handle);

/* Release handle; frees physical pages when ref_count reaches 0 */
int shm_close(uint64_t handle);

/* Get byte size of region */
uint64_t shm_size(uint64_t handle);

#endif
