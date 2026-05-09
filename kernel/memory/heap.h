#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

int heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
uint64_t heap_bytes_total(void);
uint64_t heap_bytes_used(void);

#endif
