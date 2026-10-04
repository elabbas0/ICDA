#ifndef ICDA_LIBC_STDIO_H
#define ICDA_LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF      (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ   1024

typedef struct ic_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

int   putchar(int c);
int   puts(const char *s);
int   getchar(void);
int   fputc(int c, FILE *f);
int   putc(int c, FILE *f);
int   fputs(const char *s, FILE *f);
int   fgetc(FILE *f);
int   getc(FILE *f);
char *fgets(char *buf, int cap, FILE *f);
int   ungetc(int c, FILE *f);

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
int    fflush(FILE *f);
size_t fread(void *dst, size_t size, size_t count, FILE *f);
size_t fwrite(const void *src, size_t size, size_t count, FILE *f);
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);

int remove(const char *path);
int rename(const char *from, const char *to);

#endif
