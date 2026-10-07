#ifndef ICDA_OH_POSIX_H
#define ICDA_OH_POSIX_H
#include <stdint.h>
#include <stddef.h>

typedef long          ssize_t;
typedef long          off_t;
typedef int           pid_t;
typedef long          time_t;
typedef long          suseconds_t;
typedef unsigned int  useconds_t;

struct timeval  { time_t tv_sec; suseconds_t tv_usec; };
struct timespec { time_t tv_sec; long tv_nsec; };
struct timeb    { time_t time; unsigned short millitm; short timezone, dstflag; };
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };

#ifdef __cplusplus
extern "C" {
#endif
uint32_t ic_time_ms(void);
#ifdef __cplusplus
}
#endif

static inline int gettimeofday(struct timeval *tv, void *tz) {
    uint32_t ms = ic_time_ms();
    (void)tz;
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
    return 0;
}
static inline void ftime(struct timeb *t) {
    uint32_t ms = ic_time_ms();
    t->time = ms / 1000;
    t->millitm = (unsigned short)(ms % 1000);
    t->timezone = t->dstflag = 0;
}
static inline time_t time(time_t *t) {
    time_t v = (time_t)(ic_time_ms() / 1000);
    if (t) *t = v;
    return v;
}
static inline struct tm *localtime(const time_t *t) {
    static struct tm z;
    (void)t;
    return &z;
}
static inline size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    (void)fmt;
    (void)tm;
    if (max) s[0] = 0;
    return 0;
}
static inline int usleep(useconds_t us) {
    (void)us;
    return 0;
}
#define _SC_NPROCESSORS_ONLN 84
static inline long sysconf(int name) {
    (void)name;
    return 1;
}

/* threads: one thread only */
typedef unsigned long pthread_t;
typedef struct { int locked; } pthread_mutex_t;
typedef struct { int unused; } pthread_mutexattr_t;
typedef struct { int unused; } pthread_cond_t;
typedef struct { int unused; } pthread_condattr_t;
typedef struct { int unused; } pthread_attr_t;
typedef struct { int value; } sem_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_SCOPE_SYSTEM 0
#define SCHED_FIFO 1
#define ETIMEDOUT 110
#ifndef EAGAIN
#define EAGAIN 11
#endif
#define SEM_FAILED ((sem_t *)0)

static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)a; m->locked = 0; return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { m->locked = 1; return 0; }
static inline int pthread_mutex_trylock(pthread_mutex_t *m) { m->locked = 1; return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { m->locked = 0; return 0; }
static inline int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)c; (void)a; return 0; }
static inline int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_signal(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t *c) { (void)c; return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { (void)c; (void)m; return 0; }
static inline int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) { (void)c; (void)m; (void)t; return ETIMEDOUT; }
static inline int pthread_attr_init(pthread_attr_t *a) { (void)a; return 0; }
static inline int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
static inline int pthread_attr_setscope(pthread_attr_t *a, int s) { (void)a; (void)s; return 0; }
static inline int pthread_attr_setschedpolicy(pthread_attr_t *a, int p) { (void)a; (void)p; return 0; }
static inline int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) { (void)t; (void)a; (void)fn; (void)arg; return EAGAIN; }
static inline int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }
static inline pthread_t pthread_self(void) { return 1; }
static inline int pthread_setname_np(pthread_t t, const char *n) { (void)t; (void)n; return 0; }
static inline int sem_init(sem_t *s, int p, unsigned v) { (void)p; s->value = (int)v; return 0; }
static inline int sem_destroy(sem_t *s) { (void)s; return 0; }
static inline int sem_post(sem_t *s) { s->value++; return 0; }
static inline int sem_wait(sem_t *s) { if (s->value > 0) s->value--; return 0; }
static inline int sem_trywait(sem_t *s) { if (s->value > 0) { s->value--; return 0; } return -1; }
static inline int sem_timedwait(sem_t *s, const struct timespec *t) { (void)t; return sem_trywait(s); }
static inline int sem_getvalue(sem_t *s, int *v) { *v = s->value; return 0; }
static inline sem_t *sem_open(const char *n, int f, ...) { (void)n; (void)f; return SEM_FAILED; }
static inline int sem_close(sem_t *s) { (void)s; return 0; }
static inline int sem_unlink(const char *n) { (void)n; return 0; }
static inline int sched_yield(void) { return 0; }
#ifndef EINTR
#define EINTR 4
#endif
typedef struct { unsigned long bits[16]; } cpu_set_t;
#define CPU_SETSIZE 1024
#define CPU_ZERO(s) memset((s), 0, sizeof(cpu_set_t))
#define CPU_ISSET(i, s) ((i) == 0)
#define CPU_COUNT(s) 1
static inline int sched_getaffinity(int pid, size_t n, cpu_set_t *s) { (void)pid; (void)n; (void)s; return 0; }
#endif
