#ifndef BOOTLOG_H
#define BOOTLOG_H

#include <stdint.h>

/* RAM copy of serial and console output, saved to /BOOTLOG.TXT on the
 * ICDA system partition (see bootlog.c). */
void bootlog_putc(char c);
void bootlog_puts(const char *s);
int  bootlog_flush(const char *milestone);
void bootlog_start_thread(void);
void bootlog_crash(const char *what, uint64_t vector, uint64_t rip, uint64_t addr);

#endif
