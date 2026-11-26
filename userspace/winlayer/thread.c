/* Threads, the TEB and PEB, TLS slots, handles and waitable objects.
 *
 * Each thread running Windows code has a TEB (thread environment block) at
 * GS, as on Windows: programs read their stack bounds, TLS pointer and last
 * error through gs:[...].  The kernel keeps each thread's GS base
 * (arch_prctl ARCH_SET_GS).  Windows threads are POSIX threads underneath.
 *
 * Waits: one lock and one condition variable for every waitable object, so
 * WaitForMultipleObjects looks at its objects under one lock; any change
 * wakes all waiters, who look again.  Plenty for a game's few threads. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

#define ARCH_SET_GS 0x1001

/* TEB offsets (x64) */
#define TEB_EXCEPTION_LIST 0x00
#define TEB_STACK_BASE     0x08
#define TEB_STACK_LIMIT    0x10
#define TEB_SELF           0x30
#define TEB_PID            0x40
#define TEB_TID            0x48
#define TEB_TLS_POINTER    0x58
#define TEB_PEB            0x60
#define TEB_LAST_ERROR     0x68
#define TEB_TLS_SLOTS      0x1480
#define TEB_TLS_EXPANSION  0x1780
#define TEB_OURS           0x1800      /* the wthread_t (past what Windows defines) */
#define TEB_SIZE           0x2000

struct wthread {
    uint8_t              *teb;
    pthread_t             pt;
    DWORD                 tid;
    int                   done;
    DWORD                 exit_code;
    LPTHREAD_START_ROUTINE start;
    void                 *param;
    int                   suspended;
    int                   refs;       /* its handles, and itself while running */
    void                **tls_blocks; /* a module's static TLS, by its index */
};

static uint8_t *peb;
static pthread_key_t thread_key;

void *peb_get(void) { return peb; }

static uint8_t *teb_raw(void) {
    uint8_t *t;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(t));
    return t;
}

void *teb_current(void) { return teb_raw(); }

wthread_t *thread_current(void) {
    return (wthread_t *)pthread_getspecific(thread_key);
}

DWORD thread_id(void) { return (DWORD)syscall(SYS_gettid); }

void wset_last_error(DWORD e) {
    uint8_t *t = teb_raw();
    if (t) *(DWORD *)(t + TEB_LAST_ERROR) = e;
}

DWORD wget_last_error(void) {
    uint8_t *t = teb_raw();
    return t ? *(DWORD *)(t + TEB_LAST_ERROR) : 0;
}

void *tls_slot_get(DWORD i) {
    uint8_t *t = teb_raw();
    if (i < 64) return ((void **)(t + TEB_TLS_SLOTS))[i];
    if (i < 64 + 1024) {
        void **x = *(void ***)(t + TEB_TLS_EXPANSION);
        return x ? x[i - 64] : 0;
    }
    return 0;
}

void *teb_new(wthread_t *t, void *stack_base, void *stack_limit) {
    uint8_t *teb = mmap(0, TEB_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (teb == MAP_FAILED) return 0;
    *(void **)(teb + TEB_EXCEPTION_LIST) = (void *)-1;
    *(void **)(teb + TEB_STACK_BASE) = stack_base;
    *(void **)(teb + TEB_STACK_LIMIT) = stack_limit;
    *(void **)(teb + TEB_SELF) = teb;
    *(uint64_t *)(teb + TEB_PID) = (uint64_t)getpid();
    *(uint64_t *)(teb + TEB_TID) = t ? t->tid : thread_id();
    *(void **)(teb + TEB_PEB) = peb;
    *(void **)(teb + TEB_OURS) = t;
    *(void **)(teb + TEB_TLS_EXPANSION) = calloc(1024, sizeof(void *));
    if (t) t->teb = teb;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, teb) != 0) {
        wlog("arch_prctl(ARCH_SET_GS) failed: Windows programs need it");
        return 0;
    }
    pthread_setspecific(thread_key, t);
    return teb;
}

/* ---- the PEB --------------------------------------------------------------------------
 * Programs mostly ask the API, but some read the PEB: the image base, the
 * process heap, the OS version, BeingDebugged, and the loader's lists. */

typedef struct { uint16_t Length, MaximumLength; uint32_t pad; WCHAR *Buffer; } UNICODE_STRING64;

HANDLE WINAPI GetProcessHeap(void);

static void ustr(uint8_t *at, const WCHAR *s) {
    UNICODE_STRING64 *u = (UNICODE_STRING64 *)at;
    size_t n = s ? w_len(s) : 0;
    u->Length = (uint16_t)(n * 2);
    u->MaximumLength = (uint16_t)(n * 2 + 2);
    u->Buffer = (WCHAR *)s;
}

static void peb_init(void) {
    static uint8_t ldr[0x58];
    static uint8_t params[0x400];
    static uint8_t loader_lock[40];
    peb = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* PEB_LDR_DATA: Length, Initialized, and three empty circular lists */
    *(uint32_t *)(ldr + 0) = sizeof(ldr);
    ldr[4] = 1;
    for (int i = 0; i < 3; i++) {
        void **list = (void **)(ldr + 0x10 + i * 16);
        list[0] = list;
        list[1] = list;
    }
    *(void **)(peb + 0x18) = ldr;
    *(void **)(peb + 0x20) = params;
    *(uint32_t *)(peb + 0x118) = 10;      /* Windows 10 */
    *(uint32_t *)(peb + 0x11C) = 0;
    *(uint16_t *)(peb + 0x120) = 19045;
    *(uint32_t *)(peb + 0x124) = 2;       /* VER_PLATFORM_WIN32_NT */
    *(uint32_t *)(peb + 0xB8) = 4;        /* NumberOfProcessors, refreshed by GetSystemInfo */
    *(uint32_t *)(peb + 0x2C0) = 1;       /* SessionId */
    /* the loader lock: a critical section nobody holds (code looks at its owner) */
    *(void **)loader_lock = (void *)(intptr_t)-1;
    *(void **)(peb + 0x110) = loader_lock;
    *(void **)(peb + 0x30) = GetProcessHeap();
    /* RTL_USER_PROCESS_PARAMETERS: the command line, the image, the current
     * folder and the environment, for code that reads them there */
    {
        static WCHAR cwd_w[1100];
        char cwd[1024], wcwd[1100];
        WCHAR *image = utf8_to_w_dup(g_exe_win);
        extern char **environ;
        size_t total = 1, o = 0;
        WCHAR *env;
        if (getcwd(cwd, sizeof(cwd))) {
            path_to_win(cwd, wcwd, sizeof(wcwd));
            if (wcwd[strlen(wcwd) - 1] != '\\') strcat(wcwd, "\\");
            utf8_to_w(wcwd, -1, cwd_w, 1100);
        }
        for (char **e = environ; *e; e++) total += (size_t)utf8_to_w(*e, -1, 0, 0);
        env = malloc(total * sizeof(WCHAR));
        for (char **e = environ; *e; e++) o += (size_t)utf8_to_w(*e, -1, env + o, (int)(total - o));
        env[o] = 0;
        *(uint32_t *)(params + 0) = sizeof(params);
        *(uint32_t *)(params + 4) = sizeof(params);
        *(uint32_t *)(params + 8) = 1;    /* PROCESS_PARAMETERS_NORMALIZED */
        *(HANDLE *)(params + 0x20) = h_std(0);
        *(HANDLE *)(params + 0x28) = h_std(1);
        *(HANDLE *)(params + 0x30) = h_std(2);
        ustr(params + 0x38, cwd_w);       /* CurrentDirectory.DosPath */
        ustr(params + 0x60, image);       /* ImagePathName */
        ustr(params + 0x70, g_cmdline_w); /* CommandLine */
        *(WCHAR **)(params + 0x80) = env; /* Environment */
        *(uint64_t *)(params + 0x3F0) = (uint64_t)(o + 1) * 2;   /* EnvironmentSize */
    }
}

void thread_main_init(void *stack_base, void *stack_limit) {
    wthread_t *t = calloc(1, sizeof(*t));
    pthread_key_create(&thread_key, 0);
    peb_init();
    t->tid = thread_id();
    t->pt = pthread_self();
    t->refs = 1;
    if (!teb_new(t, stack_base, stack_limit)) wfatal("Windows layer", "This system cannot run Windows programs (no GS base).");
}

/* ---- handles -------------------------------------------------------------------------- */

#define MAX_HANDLES 4096
static whandle_t handles[MAX_HANDLES];
static pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;

/* handle values: multiples of 4 from 0x40, as on Windows (0 and -1 are not handles) */
static HANDLE h_of(int i) { return (HANDLE)(uintptr_t)(0x40 + 4 * i); }

static int h_index(HANDLE h) {
    uintptr_t v = (uintptr_t)h;
    if (v < 0x40 || (v & 3) || (v - 0x40) / 4 >= MAX_HANDLES) return -1;
    return (int)((v - 0x40) / 4);
}

HANDLE h_new(int kind, void *obj) {
    pthread_mutex_lock(&hlock);
    for (int i = 1; i < MAX_HANDLES; i++) {
        if (handles[i].kind == H_FREE) {
            handles[i].kind = kind;
            handles[i].obj = obj;
            handles[i].refs = 1;
            handles[i].access = ~0u;
            pthread_mutex_unlock(&hlock);
            return h_of(i);
        }
    }
    pthread_mutex_unlock(&hlock);
    return 0;
}

whandle_t *h_get(HANDLE h, int kind) {
    int i = h_index(h);
    if (i <= 0 || handles[i].kind == H_FREE || (kind && handles[i].kind != kind)) return 0;
    return &handles[i];
}

void h_close(HANDLE h) {
    int i = h_index(h);
    if (i <= 0) return;
    pthread_mutex_lock(&hlock);
    handles[i].kind = H_FREE;
    handles[i].obj = 0;
    pthread_mutex_unlock(&hlock);
}

/* ---- waitable objects ---------------------------------------------------------------- */

static pthread_mutex_t slock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scond = PTHREAD_COND_INITIALIZER;

void sync_lock(void) { pthread_mutex_lock(&slock); }
void sync_unlock(void) { pthread_mutex_unlock(&slock); }
void sync_broadcast(void) { pthread_cond_broadcast(&scond); }

static struct timespec deadline;
static __thread struct timespec my_deadline;

int sync_wait_ms(DWORD ms) {
    (void)deadline;
    if (ms == INFINITE) return pthread_cond_wait(&scond, &slock);
    return pthread_cond_timedwait(&scond, &slock, &my_deadline);
}

void sync_deadline(DWORD ms) {
    clock_gettime(CLOCK_REALTIME, &my_deadline);
    my_deadline.tv_sec += ms / 1000;
    my_deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (my_deadline.tv_nsec >= 1000000000L) my_deadline.tv_sec++, my_deadline.tv_nsec -= 1000000000L;
}

typedef struct { int manual, signaled; } wevent_t;
typedef struct { DWORD owner; int count; } wmutex_t;
typedef struct { LONG count, max; } wsem_t;

/* signaled (and taken, when consume): under the sync lock */
int wobj_signaled(whandle_t *h, int consume) {
    switch (h->kind) {
    case H_EVENT: {
        wevent_t *e = h->obj;
        if (!e->signaled) return 0;
        if (consume && !e->manual) e->signaled = 0;
        return 1;
    }
    case H_MUTEX: {
        wmutex_t *m = h->obj;
        if (m->count && m->owner != thread_id()) return 0;
        if (consume) m->owner = thread_id(), m->count++;
        return 1;
    }
    case H_SEMAPHORE: {
        wsem_t *s = h->obj;
        if (s->count <= 0) return 0;
        if (consume) s->count--;
        return 1;
    }
    case H_THREAD:
        return ((wthread_t *)h->obj)->done;
    case H_PROCESS:
        return ((wproc_t *)h->obj)->done;
    default:
        return -1;
    }
}

/* ---- names ----------------------------------------------------------------------------
 * A named event, mutex, semaphore or waitable timer is found by its name while
 * a handle to it is open, as on Windows; the names are this process's (ICDA's
 * Windows programs share no objects between processes).  "Global\" and
 * "Local\" are the same place here: there is one session.  A timer is an
 * event inside, with names of its own (ns: NS_TIMER). */

typedef struct named {
    struct named *next;
    int           ns;
    void         *obj;
    int           handles;
    WCHAR         name[];
} named_t;

static named_t *names;
static pthread_mutex_t nlock = PTHREAD_MUTEX_INITIALIZER;

static const WCHAR *after(const WCHAR *n, const char *prefix) {
    for (; *prefix; prefix++, n++)
        if (*n != (WCHAR)*prefix) return 0;
    return n;
}

static const WCHAR *name_part(const WCHAR *n) {
    const WCHAR *p = after(n, "Global\\");
    if (!p) p = after(n, "Local\\");
    return p ? p : n;
}

static named_t *named_find(const WCHAR *name) {
    const WCHAR *n = name_part(name);
    for (named_t *e = names; e; e = e->next)
        if (!w_cmp(e->name, n)) return e;
    return 0;
}

/* a new object's handle, or under a name in use the old object's
 * (ERROR_ALREADY_EXISTS; obj is freed); a name used by another kind fails
 * with ERROR_INVALID_HANDLE */
HANDLE h_named(int kind, int ns, const WCHAR *name, void *obj) {
    named_t *e;
    HANDLE h;
    if (!obj) {
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (!name || !*name) {
        h = h_new(kind, obj);
        if (!h) free(obj);
        else wset_last_error(0);
        return h;
    }
    pthread_mutex_lock(&nlock);
    e = named_find(name);
    if (e) {
        free(obj);
        if (e->ns != ns) {
            pthread_mutex_unlock(&nlock);
            wset_last_error(ERROR_INVALID_HANDLE);
            return 0;
        }
        h = h_new(kind, e->obj);
        if (h) e->handles++;
        pthread_mutex_unlock(&nlock);
        if (h) wset_last_error(ERROR_ALREADY_EXISTS);
        return h;
    }
    e = calloc(1, sizeof(*e) + (w_len(name_part(name)) + 1) * sizeof(WCHAR));
    h = e ? h_new(kind, obj) : 0;
    if (!h) {
        pthread_mutex_unlock(&nlock);
        free(e), free(obj);
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    e->ns = ns, e->obj = obj, e->handles = 1;
    memcpy(e->name, name_part(name), (w_len(name_part(name)) + 1) * sizeof(WCHAR));
    e->next = names, names = e;
    pthread_mutex_unlock(&nlock);
    wset_last_error(0);
    return h;
}

/* OpenEvent and the others: no such name is ERROR_FILE_NOT_FOUND */
HANDLE h_open_named(int kind, int ns, const WCHAR *name) {
    named_t *e;
    HANDLE h = 0;
    if (!name) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&nlock);
    e = named_find(name);
    if (e && e->ns == ns) {
        h = h_new(kind, e->obj);
        if (h) e->handles++;
    }
    pthread_mutex_unlock(&nlock);
    if (!h) wset_last_error(e ? ERROR_INVALID_HANDLE : ERROR_FILE_NOT_FOUND);
    return h;
}

/* a handle to obj opened (DuplicateHandle, +1) or closed (-1): the name goes
 * with the last one */
void named_ref(void *obj, int delta) {
    pthread_mutex_lock(&nlock);
    for (named_t **p = &names; *p; p = &(*p)->next)
        if ((*p)->obj == obj) {
            named_t *e = *p;
            e->handles += delta;
            if (e->handles <= 0) *p = e->next, free(e);
            break;
        }
    pthread_mutex_unlock(&nlock);
}

HANDLE event_new(BOOL manual, BOOL initial, const WCHAR *name, int ns) {
    wevent_t *e = calloc(1, sizeof(*e));
    if (e) e->manual = manual, e->signaled = initial;
    return h_named(H_EVENT, ns, name, e);
}

HANDLE WINAPI CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name) {
    (void)sa;
    return event_new(manual, initial, name, H_EVENT);
}

/* the A names: UTF-8 */
static HANDLE create_a(HANDLE (*make)(void *, const WCHAR *, void *), void *sa, const char *name, void *ctx) {
    WCHAR *w = name ? utf8_to_w_dup(name) : 0;
    HANDLE h = make(sa, w, ctx);
    free(w);
    return h;
}

typedef struct { BOOL manual, initial; } event_args_t;
static HANDLE make_event(void *sa, const WCHAR *name, void *ctx) {
    event_args_t *a = ctx;
    return CreateEventW(sa, a->manual, a->initial, name);
}

HANDLE WINAPI CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name) {
    event_args_t a = { manual, initial };
    return create_a(make_event, sa, name, &a);
}

HANDLE WINAPI OpenEventW(DWORD access, BOOL inherit, const WCHAR *name) {
    (void)access, (void)inherit;
    return h_open_named(H_EVENT, H_EVENT, name);
}

HANDLE WINAPI CreateEventExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) {
    (void)access;
    return CreateEventW(sa, (flags & 1) != 0, (flags & 2) != 0, name);
}

BOOL WINAPI SetEvent(HANDLE h) {
    whandle_t *w = h_get(h, H_EVENT);
    if (!w) return FALSE;
    sync_lock();
    ((wevent_t *)w->obj)->signaled = 1;
    sync_broadcast();
    sync_unlock();
    return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE h) {
    whandle_t *w = h_get(h, H_EVENT);
    if (!w) return FALSE;
    sync_lock();
    ((wevent_t *)w->obj)->signaled = 0;
    sync_unlock();
    return TRUE;
}

BOOL WINAPI PulseEvent(HANDLE h) {
    SetEvent(h);
    return ResetEvent(h);
}

/* asked to own a mutex that already exists, the caller does not get it */
HANDLE WINAPI CreateMutexW(void *sa, BOOL owned, const WCHAR *name) {
    wmutex_t *m = calloc(1, sizeof(*m));
    HANDLE h;
    (void)sa;
    if (!m) {
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    h = h_named(H_MUTEX, H_MUTEX, name, m);
    if (h && owned && wget_last_error() != ERROR_ALREADY_EXISTS) {
        m->owner = thread_id(), m->count = 1;
        wset_last_error(0);
    }
    return h;
}

static HANDLE make_mutex(void *sa, const WCHAR *name, void *ctx) { return CreateMutexW(sa, *(BOOL *)ctx, name); }

HANDLE WINAPI CreateMutexA(void *sa, BOOL owned, const char *name) { return create_a(make_mutex, sa, name, &owned); }

HANDLE WINAPI OpenMutexW(DWORD access, BOOL inherit, const WCHAR *name) {
    (void)access, (void)inherit;
    return h_open_named(H_MUTEX, H_MUTEX, name);
}

BOOL WINAPI ReleaseMutex(HANDLE h) {
    whandle_t *w = h_get(h, H_MUTEX);
    wmutex_t *m;
    if (!w) return FALSE;
    m = w->obj;
    sync_lock();
    if (!m->count || m->owner != thread_id()) {
        sync_unlock();
        wset_last_error(288);   /* ERROR_NOT_OWNER */
        return FALSE;
    }
    if (--m->count == 0) m->owner = 0;
    sync_broadcast();
    sync_unlock();
    return TRUE;
}

HANDLE WINAPI CreateSemaphoreW(void *sa, LONG initial, LONG max, const WCHAR *name) {
    wsem_t *s;
    (void)sa;
    if (max <= 0 || initial < 0 || initial > max) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    s = calloc(1, sizeof(*s));
    if (s) s->count = initial, s->max = max;
    return h_named(H_SEMAPHORE, H_SEMAPHORE, name, s);
}

typedef struct { LONG initial, max; } sem_args_t;
static HANDLE make_sem(void *sa, const WCHAR *name, void *ctx) {
    sem_args_t *a = ctx;
    return CreateSemaphoreW(sa, a->initial, a->max, name);
}

HANDLE WINAPI CreateSemaphoreA(void *sa, LONG initial, LONG max, const char *name) {
    sem_args_t a = { initial, max };
    return create_a(make_sem, sa, name, &a);
}

HANDLE WINAPI OpenSemaphoreW(DWORD access, BOOL inherit, const WCHAR *name) {
    (void)access, (void)inherit;
    return h_open_named(H_SEMAPHORE, H_SEMAPHORE, name);
}

HANDLE WINAPI OpenSemaphoreA(DWORD access, BOOL inherit, const char *name) {
    WCHAR *w = name ? utf8_to_w_dup(name) : 0;
    HANDLE h = OpenSemaphoreW(access, inherit, w);
    free(w);
    return h;
}

HANDLE WINAPI CreateSemaphoreExW(void *sa, LONG initial, LONG max, const WCHAR *name, DWORD flags, DWORD access) {
    (void)flags, (void)access;
    return CreateSemaphoreW(sa, initial, max, name);
}

BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG n, LONG *prev) {
    whandle_t *w = h_get(h, H_SEMAPHORE);
    wsem_t *s;
    if (!w) return FALSE;
    s = w->obj;
    sync_lock();
    if (prev) *prev = s->count;
    if (s->count + n > s->max) {
        sync_unlock();
        wset_last_error(298);   /* ERROR_TOO_MANY_POSTS */
        return FALSE;
    }
    s->count += n;
    sync_broadcast();
    sync_unlock();
    return TRUE;
}

DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    whandle_t *w[64];
    if (n == 0 || n > 64) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    for (DWORD i = 0; i < n; i++) {
        w[i] = h_get(hs[i], 0);
        if (!w[i]) {
            wset_last_error(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
    }
    if (ms != INFINITE) sync_deadline(ms);
    sync_lock();
    for (;;) {
        if (all) {
            DWORD ok = 0;
            for (DWORD i = 0; i < n; i++) ok += wobj_signaled(w[i], 0) > 0;
            if (ok == n) {
                for (DWORD i = 0; i < n; i++) (void)wobj_signaled(w[i], 1);
                sync_unlock();
                return WAIT_OBJECT_0;
            }
        } else {
            for (DWORD i = 0; i < n; i++) {
                if (wobj_signaled(w[i], 1) > 0) {
                    sync_unlock();
                    return WAIT_OBJECT_0 + i;
                }
            }
        }
        if (ms == 0 || sync_wait_ms(ms) == ETIMEDOUT) {
            sync_unlock();
            return WAIT_TIMEOUT;
        }
    }
}

DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) {
    if (h == (HANDLE)(intptr_t)-1) {           /* the process: never, in its own threads */
        if (ms == INFINITE) for (;;) pause();
        usleep((useconds_t)ms * 1000);
        return WAIT_TIMEOUT;
    }
    return WaitForMultipleObjects(1, &h, FALSE, ms);
}

DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable) {
    (void)alertable;
    return WaitForSingleObject(h, ms);
}

DWORD WINAPI WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable) {
    (void)alertable;
    return WaitForMultipleObjects(n, hs, all, ms);
}

DWORD WINAPI SignalObjectAndWait(HANDLE sig, HANDLE wait, DWORD ms, BOOL alertable) {
    whandle_t *w = h_get(sig, 0);
    (void)alertable;
    if (w && w->kind == H_EVENT) SetEvent(sig);
    else if (w && w->kind == H_MUTEX) ReleaseMutex(sig);
    else if (w && w->kind == H_SEMAPHORE) ReleaseSemaphore(sig, 1, 0);
    return WaitForSingleObject(wait, ms);
}

/* ---- threads -------------------------------------------------------------------------- */

void tls_thread_blocks(wthread_t *t);       /* loader.c: the static TLS of every module */
void crash_handler_thread(void);            /* main.c */

void tls_thread_gone(wthread_t *t);        /* loader.c */

static void thread_finish(wthread_t *t, DWORD code) {
    ld_thread_attach(0);
    tls_thread_gone(t);
    sync_lock();
    t->exit_code = code;
    t->done = 1;
    sync_broadcast();
    sync_unlock();
}

static void *thread_trampoline(void *arg) {
    wthread_t *t = arg;
    pthread_attr_t a;
    void *addr = 0;
    size_t size = 0;
    DWORD rc;
    t->tid = thread_id();
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        pthread_attr_getstack(&a, &addr, &size);
        pthread_attr_destroy(&a);
    }
    if (!teb_new(t, (uint8_t *)addr + size, addr)) return 0;
    crash_handler_thread();
    tls_thread_blocks(t);
    sync_lock();
    while (t->suspended) sync_wait_ms(INFINITE);
    sync_unlock();
    ld_thread_attach(1);
    rc = t->start(t->param);
    thread_finish(t, rc);
    return 0;
}

/* a thread the layer made (sound, timers) that calls into the program */
void win_thread_setup(void) {
    wthread_t *t;
    pthread_attr_t a;
    void *addr = 0;
    size_t size = 0;
    if (thread_current()) return;
    t = calloc(1, sizeof(*t));
    t->tid = thread_id();
    t->pt = pthread_self();
    t->refs = 1;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        pthread_attr_getstack(&a, &addr, &size);
        pthread_attr_destroy(&a);
    }
    teb_new(t, (uint8_t *)addr + size, addr);
    crash_handler_thread();
    tls_thread_blocks(t);
}

HANDLE WINAPI CreateThread(void *sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, void *param, DWORD flags, DWORD *tid) {
    wthread_t *t = calloc(1, sizeof(*t));
    pthread_attr_t a;
    HANDLE h;
    (void)sa;
    if (!t) return 0;
    t->start = start;
    t->param = param;
    t->suspended = (flags & 4) != 0;      /* CREATE_SUSPENDED */
    t->refs = 2;
    h = h_new(H_THREAD, t);
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack > (1 << 20) ? stack : (size_t)8 << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t->pt, &a, thread_trampoline, t) != 0) {
        pthread_attr_destroy(&a);
        h_close(h);
        free(t);
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    pthread_attr_destroy(&a);
    /* its id is known once it runs: wait for that */
    for (int i = 0; i < 2000 && !__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE); i++) usleep(50);
    if (tid) *tid = t->tid;
    return h;
}

DWORD WINAPI ResumeThread(HANDLE h) {
    whandle_t *w = h_get(h, H_THREAD);
    DWORD was;
    if (!w) return (DWORD)-1;
    sync_lock();
    was = (DWORD)((wthread_t *)w->obj)->suspended;
    ((wthread_t *)w->obj)->suspended = 0;
    sync_broadcast();
    sync_unlock();
    return was;
}

DWORD WINAPI SuspendThread(HANDLE h) {
    (void)h;
    return 0;
}

void WINAPI ExitThread(DWORD code) {
    wthread_t *t = thread_current();
    if (t) thread_finish(t, code);
    pthread_exit(0);
}

BOOL WINAPI GetExitCodeThread(HANDLE h, DWORD *code) {
    whandle_t *w = h_get(h, H_THREAD);
    if (!w) return FALSE;
    *code = ((wthread_t *)w->obj)->done ? ((wthread_t *)w->obj)->exit_code : 259;   /* STILL_ACTIVE */
    return TRUE;
}

BOOL WINAPI TerminateThread(HANDLE h, DWORD code) {
    (void)h, (void)code;
    wset_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

DWORD WINAPI GetThreadId(HANDLE h) {
    whandle_t *w;
    if (h == (HANDLE)(intptr_t)-2) return thread_id();
    w = h_get(h, H_THREAD);
    return w ? ((wthread_t *)w->obj)->tid : 0;
}

DWORD WINAPI GetCurrentThreadId(void) { return thread_id(); }
DWORD WINAPI GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE WINAPI GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
HANDLE WINAPI GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }

/* ---- TLS slots ------------------------------------------------------------------------- */

static uint8_t slot_used[64 + 1024];

DWORD WINAPI TlsAlloc(void) {
    pthread_mutex_lock(&hlock);
    for (DWORD i = 0; i < sizeof(slot_used); i++) {
        if (!slot_used[i]) {
            slot_used[i] = 1;
            pthread_mutex_unlock(&hlock);
            return i;
        }
    }
    pthread_mutex_unlock(&hlock);
    return 0xFFFFFFFF;
}

BOOL WINAPI TlsFree(DWORD i) {
    if (i >= sizeof(slot_used)) return FALSE;
    slot_used[i] = 0;
    return TRUE;
}

void *WINAPI TlsGetValue(DWORD i) {
    wset_last_error(0);
    return tls_slot_get(i);
}

BOOL WINAPI TlsSetValue(DWORD i, void *v) {
    uint8_t *t = teb_raw();
    if (i < 64) {
        ((void **)(t + TEB_TLS_SLOTS))[i] = v;
        return TRUE;
    }
    if (i < 64 + 1024) {
        void **x = *(void ***)(t + TEB_TLS_EXPANSION);
        if (x) x[i - 64] = v;
        return x != 0;
    }
    return FALSE;
}

/* fiber-local storage: like TLS here (no fibers) */
DWORD WINAPI FlsAlloc(void *cb) {
    (void)cb;
    return TlsAlloc();
}
BOOL WINAPI FlsFree(DWORD i) { return TlsFree(i); }
void *WINAPI FlsGetValue(DWORD i) { return TlsGetValue(i); }
BOOL WINAPI FlsSetValue(DWORD i, void *v) { return TlsSetValue(i, v); }

/* the module TLS array (TEB.ThreadLocalStoragePointer) */
void teb_set_tls_pointer(void **blocks) {
    *(void ***)(teb_raw() + TEB_TLS_POINTER) = blocks;
}

void **thread_tls_blocks(wthread_t *t) { return t ? t->tls_blocks : 0; }
void thread_set_tls_blocks(wthread_t *t, void **b) {
    if (t) t->tls_blocks = b;
}
