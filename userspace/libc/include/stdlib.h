#ifndef ICDA_LIBC_STDLIB_H
#define ICDA_LIBC_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7FFFFFFF

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

void  exit(int status) __attribute__((noreturn));
void  abort(void) __attribute__((noreturn));
int   atexit(void (*fn)(void));

int                atoi(const char *s);
long               atol(const char *s);
long               strtol(const char *s, char **end, int base);
unsigned long      strtoul(const char *s, char **end, int base);
long long          strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double             strtod(const char *s, char **end);
double             atof(const char *s);

int  abs(int v);
long labs(long v);

int  rand(void);
void srand(unsigned seed);

void  qsort(void *base, size_t count, size_t size, int (*cmp)(const void *, const void *));
char *getenv(const char *name);

#endif
