/* ic_mem for Linux programs: ICDA's graphics code allocates through these;
 * here they are the C library's allocator. */
#include <malloc.h>
#include <stdlib.h>
#include "ic_mem.h"

void *ic_malloc(uint64_t size) { return malloc(size); }
void *ic_calloc(uint64_t count, uint64_t size) { return calloc(count, size); }
void *ic_realloc(void *ptr, uint64_t size) { return realloc(ptr, size); }
void ic_free(void *ptr) { free(ptr); }
uint64_t ic_mem_usable(const void *ptr) { return ptr ? malloc_usable_size((void *)ptr) : 0; }
