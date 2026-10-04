#ifndef IPC_SHM_H
#define IPC_SHM_H

#include <stdint.h>






#define SHM_VIRT_BASE   0x0000010000000000ULL
#define SHM_SLOT_SIZE   (16ULL * 1024ULL * 1024ULL)
#define SHM_MAX_REGIONS 32


uint64_t shm_create(uint64_t size);


uint64_t shm_map(uint64_t handle);


int shm_unmap(uint64_t handle);


int shm_close(uint64_t handle);


struct process;
void shm_proc_exit(struct process *proc);


uint64_t shm_size(uint64_t handle);

#endif
