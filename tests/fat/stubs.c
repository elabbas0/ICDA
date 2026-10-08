/* what kernel/fs/fatfs.c needs from the kernel, for the host test */
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include "../../kernel/drivers/rtc/rtc.h"
void *kmalloc(size_t n) { return calloc(1, n ? n : 1); }
void kfree(void *p) { free(p); }
int rtc_read(rtc_time_t *t) { (void)t; return -1; }
void sched_preempt_disable(void) {}
void sched_preempt_enable(void) {}
