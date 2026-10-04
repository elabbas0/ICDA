#ifndef USERSPACE_IC_MEM_H
#define USERSPACE_IC_MEM_H

#include <stdint.h>

void *ic_malloc(uint64_t size);

void *ic_calloc(uint64_t count, uint64_t size);

void *ic_realloc(void *ptr, uint64_t size);

void  ic_free(void *ptr);

uint64_t ic_mem_usable(const void *ptr);

#endif
