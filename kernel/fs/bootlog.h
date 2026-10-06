#ifndef BOOTLOG_H
#define BOOTLOG_H

/* RAM copy of serial and console output, saved to /BOOTLOG.TXT on the
 * ICDA system partition (see bootlog.c). */
void bootlog_putc(char c);
void bootlog_puts(const char *s);
int  bootlog_flush(const char *milestone);
void bootlog_start_thread(void);

#endif
