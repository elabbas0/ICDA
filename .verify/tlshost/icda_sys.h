#ifndef HOST_ICDA_SYS_H
#define HOST_ICDA_SYS_H
#include <stdint.h>
uint64_t icda_ticks(void);
void icda_sleep(uint64_t t);
long icda_write_file(const char *p, const void *b, unsigned long n);
static inline long icda_read_file(const char *p, char *b, unsigned long n) { (void)p; (void)b; (void)n; return -1; }
#endif
