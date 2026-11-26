/* The Windows thread pool (Vista's API): work, timer and wait objects whose
 * callbacks run on worker threads, as std::async, Microsoft's C++ library
 * (msvcp140) and many programs use them; and the older QueueUserWorkItem.
 *
 * Callbacks are queued to a set of worker threads of ours (each with a TEB,
 * as Windows code needs), grown when all are busy, up to 64.  A timer
 * object has a thread that waits until it is due (then queues its callback,
 * again each period); a wait object one that waits for its handle.  Each
 * object counts its callbacks queued or running, for
 * WaitForThreadpool*Callbacks and for freeing it after CloseThreadpool*.
 * The callback instance carries what is to happen when the callback
 * returns (SetEventWhenCallbackReturns and the like).  Pools and their
 * limits are accepted and otherwise not separate; cleanup groups close
 * their members. */
#define _GNU_SOURCE
#include "win.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

typedef struct { DWORD LowPart; DWORD HighPart; } FT;
typedef struct critical_section CRITICAL_SECTION_;

BOOL WINAPI SetEvent(HANDLE h);
BOOL WINAPI ReleaseMutex(HANDLE h);
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG n, LONG *prev);
BOOL WINAPI FreeLibrary(HMODULE h);
void WINAPI LeaveCriticalSection(void *cs);
HANDLE WINAPI CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name);
BOOL WINAPI CloseHandle(HANDLE h);
DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);

enum { TP_WORK = 1, TP_TIMER, TP_WAIT, TP_SIMPLE };

typedef struct tp_obj {
    int              kind;
    void            *cb, *ctx;
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
    int              pending;          /* callbacks queued or running */
    int              closing;
    struct tp_obj   *group_next;       /* in its cleanup group */
    struct tp_group *group;
    /* timer */
    int              armed, has_thread;
    uint64_t         due_ns;           /* CLOCK_MONOTONIC */
    DWORD            period;
    /* wait */
    HANDLE           handle, cancel;
    DWORD            timeout_ms;
    uint64_t         wait_gen;
} tp_obj_t;

typedef struct tp_group {
    pthread_mutex_t mu;
    tp_obj_t       *members;
} tp_group_t;

/* what a callback's instance does when it returns */
typedef struct {
    HANDLE event, mutex, semaphore;
    LONG   sem_count;
    void  *critsec;
    HMODULE library;
} tp_instance_t;

/* ---- the workers ------------------------------------------------------------------------------- */

typedef struct item {
    struct item *next;
    tp_obj_t    *obj;
    DWORD        result;              /* a wait's: WAIT_OBJECT_0 or WAIT_TIMEOUT */
} item_t;

static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;
static item_t *qhead, *qtail;
static int workers, idle;

static uint64_t mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void obj_done(tp_obj_t *o) {
    int free_it;
    pthread_mutex_lock(&o->mu);
    o->pending--;
    free_it = o->closing && o->pending == 0 && !o->has_thread;
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
    if (free_it) {
        pthread_mutex_destroy(&o->mu);
        pthread_cond_destroy(&o->cv);
        free(o);
    }
}

static void run(item_t *it) {
    tp_obj_t *o = it->obj;
    tp_instance_t inst;
    memset(&inst, 0, sizeof(inst));
    switch (o->kind) {
    case TP_WORK:
        ((void (WCALLBACK *)(void *, void *, void *))o->cb)(&inst, o->ctx, o);
        break;
    case TP_TIMER:
        ((void (WCALLBACK *)(void *, void *, void *))o->cb)(&inst, o->ctx, o);
        break;
    case TP_WAIT:
        ((void (WCALLBACK *)(void *, void *, void *, DWORD))o->cb)(&inst, o->ctx, o, it->result);
        break;
    case TP_SIMPLE:
        ((void (WCALLBACK *)(void *, void *))o->cb)(&inst, o->ctx);
        break;
    }
    if (inst.critsec) LeaveCriticalSection(inst.critsec);
    if (inst.mutex) ReleaseMutex(inst.mutex);
    if (inst.semaphore) ReleaseSemaphore(inst.semaphore, inst.sem_count, 0);
    if (inst.event) SetEvent(inst.event);
    if (inst.library) FreeLibrary(inst.library);
    obj_done(o);
    if (o->kind == TP_SIMPLE) {
        /* a one-shot object: nobody else holds it */
    }
}

static void *worker(void *arg) {
    (void)arg;
    win_thread_setup();
    for (;;) {
        item_t *it;
        pthread_mutex_lock(&qmu);
        idle++;
        while (!qhead) pthread_cond_wait(&qcv, &qmu);
        idle--;
        it = qhead;
        qhead = it->next;
        if (!qhead) qtail = 0;
        pthread_mutex_unlock(&qmu);
        run(it);
        free(it);
    }
    return 0;
}

static void enqueue(tp_obj_t *o, DWORD result) {
    item_t *it = calloc(1, sizeof(*it));
    int spawn;
    if (!it) return;
    it->obj = o;
    it->result = result;
    pthread_mutex_lock(&o->mu);
    o->pending++;
    pthread_mutex_unlock(&o->mu);
    pthread_mutex_lock(&qmu);
    if (qtail) qtail->next = it;
    else qhead = it;
    qtail = it;
    spawn = idle == 0 && workers < 64;
    if (spawn) workers++;
    pthread_cond_signal(&qcv);
    pthread_mutex_unlock(&qmu);
    if (spawn) {
        pthread_t th;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 1u << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &a, worker, 0) != 0) {
            pthread_mutex_lock(&qmu);
            workers--;
            pthread_mutex_unlock(&qmu);
        }
        pthread_attr_destroy(&a);
    }
}

/* queued callbacks of o not started: dropped */
static void unqueue(tp_obj_t *o) {
    int dropped = 0;
    pthread_mutex_lock(&qmu);
    for (item_t **p = &qhead; *p;) {
        if ((*p)->obj == o) {
            item_t *it = *p;
            *p = it->next;
            free(it);
            dropped++;
        } else {
            p = &(*p)->next;
        }
    }
    qtail = 0;
    for (item_t *it = qhead; it; it = it->next) qtail = it;
    pthread_mutex_unlock(&qmu);
    for (int i = 0; i < dropped; i++) obj_done(o);
}

static void wait_callbacks(tp_obj_t *o, BOOL cancel) {
    if (!o) return;
    if (cancel) unqueue(o);
    pthread_mutex_lock(&o->mu);
    while (o->pending > 0) pthread_cond_wait(&o->cv, &o->mu);
    pthread_mutex_unlock(&o->mu);
}

/* ---- objects ------------------------------------------------------------------------------------ */

/* TP_CALLBACK_ENVIRON (version 3): its cleanup group is at offset 16 */
static tp_group_t *env_group(const void *env) { return env ? *(tp_group_t *const *)((const uint8_t *)env + 16) : 0; }

static tp_obj_t *new_obj(int kind, void *cb, void *ctx, const void *env) {
    tp_obj_t *o = calloc(1, sizeof(*o));
    tp_group_t *g = env_group(env);
    if (!o) {
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    o->kind = kind, o->cb = cb, o->ctx = ctx;
    pthread_mutex_init(&o->mu, 0);
    pthread_cond_init(&o->cv, 0);
    if (g) {
        pthread_mutex_lock(&g->mu);
        o->group = g;
        o->group_next = g->members;
        g->members = o;
        pthread_mutex_unlock(&g->mu);
    }
    return o;
}

static void close_obj(tp_obj_t *o) {
    int free_it;
    if (!o) return;
    if (o->group) {
        pthread_mutex_lock(&o->group->mu);
        for (tp_obj_t **p = &o->group->members; *p; p = &(*p)->group_next)
            if (*p == o) {
                *p = o->group_next;
                break;
            }
        pthread_mutex_unlock(&o->group->mu);
        o->group = 0;
    }
    pthread_mutex_lock(&o->mu);
    o->closing = 1;
    o->armed = 0;
    if (o->cancel) SetEvent(o->cancel);
    pthread_cond_broadcast(&o->cv);
    free_it = o->pending == 0 && !o->has_thread;
    pthread_mutex_unlock(&o->mu);
    if (free_it) {
        pthread_mutex_destroy(&o->mu);
        pthread_cond_destroy(&o->cv);
        free(o);
    }
}

void *WINAPI CreateThreadpoolWork(void *cb, void *ctx, void *env) { return new_obj(TP_WORK, cb, ctx, env); }
void WINAPI SubmitThreadpoolWork(void *w) { if (w) enqueue(w, 0); }
void WINAPI WaitForThreadpoolWorkCallbacks(void *w, BOOL cancel) { wait_callbacks(w, cancel); }
void WINAPI CloseThreadpoolWork(void *w) { close_obj(w); }

BOOL WINAPI TrySubmitThreadpoolCallback(void *cb, void *ctx, void *env) {
    tp_obj_t *o = new_obj(TP_SIMPLE, cb, ctx, 0);
    (void)env;
    if (!o) return FALSE;
    o->closing = 1;                  /* freed when its callback has run */
    enqueue(o, 0);
    return TRUE;
}

/* QueueUserWorkItem(fn, ctx, flags): fn(ctx) on a worker */
typedef struct { LPTHREAD_START_ROUTINE fn; void *ctx; } qui_t;
static void WCALLBACK qui_run(void *inst, void *arg) {
    qui_t *q = arg;
    (void)inst;
    q->fn(q->ctx);
    free(q);
}
BOOL WINAPI QueueUserWorkItem(LPTHREAD_START_ROUTINE fn, void *ctx, ULONG flags) {
    qui_t *q = malloc(sizeof(*q));
    (void)flags;
    if (!q) return FALSE;
    q->fn = fn, q->ctx = ctx;
    if (TrySubmitThreadpoolCallback((void *)qui_run, q, 0)) return TRUE;
    free(q);
    return FALSE;
}

/* ---- timers -------------------------------------------------------------------------------------- */

static void *timer_thread(void *arg) {
    tp_obj_t *o = arg;
    pthread_mutex_lock(&o->mu);
    for (;;) {
        if (o->closing) break;
        if (!o->armed) {
            pthread_cond_wait(&o->cv, &o->mu);
            continue;
        }
        {
            uint64_t now = mono_now();
            if (now < o->due_ns) {
                struct timespec ts;
                uint64_t until = o->due_ns;
                clock_gettime(CLOCK_REALTIME, &ts);
                {
                    uint64_t wait = until - now;
                    uint64_t ns = (uint64_t)ts.tv_nsec + wait % 1000000000ull;
                    ts.tv_sec += (time_t)(wait / 1000000000ull + ns / 1000000000ull);
                    ts.tv_nsec = (long)(ns % 1000000000ull);
                }
                pthread_cond_timedwait(&o->cv, &o->mu, &ts);
                continue;
            }
        }
        /* due: its callback, and again each period */
        if (o->period) o->due_ns += (uint64_t)o->period * 1000000ull;
        else o->armed = 0;
        pthread_mutex_unlock(&o->mu);
        enqueue(o, 0);
        pthread_mutex_lock(&o->mu);
    }
    o->has_thread = 0;
    {
        int free_it = o->closing && o->pending == 0;
        pthread_mutex_unlock(&o->mu);
        if (free_it) {
            pthread_mutex_destroy(&o->mu);
            pthread_cond_destroy(&o->cv);
            free(o);
        }
    }
    return 0;
}

/* a FILETIME due time: negative, relative (100 ns units); positive, absolute */
static uint64_t due_from(const FT *due) {
    int64_t v = (int64_t)((uint64_t)due->HighPart << 32 | due->LowPart);
    uint64_t now = mono_now();
    if (v <= 0) return now + (uint64_t)(-v) * 100;
    {
        uint64_t wall = now_100ns();
        return (uint64_t)v > wall ? now + ((uint64_t)v - wall) * 100 : now;
    }
}

void *WINAPI CreateThreadpoolTimer(void *cb, void *ctx, void *env) { return new_obj(TP_TIMER, cb, ctx, env); }

void WINAPI SetThreadpoolTimer(void *t, const FT *due, DWORD period, DWORD window) {
    tp_obj_t *o = t;
    (void)window;
    if (!o) return;
    pthread_mutex_lock(&o->mu);
    if (!due) {
        o->armed = 0;                 /* no more callbacks; those queued stay */
    } else {
        o->due_ns = due_from(due);
        o->period = period;
        o->armed = 1;
        if (!o->has_thread) {
            pthread_t th;
            pthread_attr_t a;
            pthread_attr_init(&a);
            pthread_attr_setstacksize(&a, 256 * 1024);
            pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&th, &a, timer_thread, o) == 0) o->has_thread = 1;
            pthread_attr_destroy(&a);
        }
    }
    pthread_cond_broadcast(&o->cv);
    pthread_mutex_unlock(&o->mu);
}

BOOL WINAPI IsThreadpoolTimerSet(void *t) {
    tp_obj_t *o = t;
    BOOL r;
    if (!o) return FALSE;
    pthread_mutex_lock(&o->mu);
    r = o->armed;
    pthread_mutex_unlock(&o->mu);
    return r;
}

void WINAPI WaitForThreadpoolTimerCallbacks(void *t, BOOL cancel) {
    tp_obj_t *o = t;
    if (o && cancel) {
        pthread_mutex_lock(&o->mu);
        o->armed = 0;
        pthread_cond_broadcast(&o->cv);
        pthread_mutex_unlock(&o->mu);
    }
    wait_callbacks(o, cancel);
}
void WINAPI CloseThreadpoolTimer(void *t) { close_obj(t); }

/* ---- waits ---------------------------------------------------------------------------------------- */

typedef struct { tp_obj_t *o; HANDLE h, cancel; DWORD ms; } wait_arg_t;

static void *wait_thread(void *arg) {
    wait_arg_t *w = arg;
    HANDLE hs[2] = { w->cancel, w->h };
    DWORD r;
    win_thread_setup();
    r = WaitForMultipleObjects(2, hs, FALSE, w->ms);
    if (r == WAIT_OBJECT_0 + 1 || r == WAIT_TIMEOUT) enqueue(w->o, r == WAIT_TIMEOUT ? WAIT_TIMEOUT : WAIT_OBJECT_0);
    obj_done(w->o);                    /* the waiter's own count */
    CloseHandle(w->cancel);
    free(w);
    return 0;
}

void *WINAPI CreateThreadpoolWait(void *cb, void *ctx, void *env) { return new_obj(TP_WAIT, cb, ctx, env); }

void WINAPI SetThreadpoolWait(void *wt, HANDLE h, const FT *timeout) {
    tp_obj_t *o = wt;
    wait_arg_t *w;
    if (!o) return;
    pthread_mutex_lock(&o->mu);
    if (o->cancel) {                  /* the wait set before ends */
        SetEvent(o->cancel);
        o->cancel = 0;
    }
    pthread_mutex_unlock(&o->mu);
    if (!h) return;
    w = calloc(1, sizeof(*w));
    if (!w) return;
    w->o = o, w->h = h;
    w->cancel = CreateEventW(0, TRUE, FALSE, 0);
    if (!timeout) w->ms = INFINITE;
    else {
        uint64_t due = due_from(timeout), now = mono_now();
        w->ms = due > now ? (DWORD)((due - now) / 1000000ull) : 0;
    }
    pthread_mutex_lock(&o->mu);
    o->cancel = w->cancel;
    o->pending++;                      /* the waiter counts until it is done */
    pthread_mutex_unlock(&o->mu);
    {
        pthread_t th;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 256 * 1024);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &a, wait_thread, w) != 0) {
            obj_done(o);
            CloseHandle(w->cancel);
            free(w);
        }
        pthread_attr_destroy(&a);
    }
}

void WINAPI WaitForThreadpoolWaitCallbacks(void *wt, BOOL cancel) {
    tp_obj_t *o = wt;
    if (o && cancel) {
        pthread_mutex_lock(&o->mu);
        if (o->cancel) SetEvent(o->cancel);
        o->cancel = 0;
        pthread_mutex_unlock(&o->mu);
    }
    wait_callbacks(o, cancel);
}
void WINAPI CloseThreadpoolWait(void *wt) { close_obj(wt); }

/* ---- callback instances -------------------------------------------------------------------------------- */

void WINAPI SetEventWhenCallbackReturns(tp_instance_t *i, HANDLE e) { if (i) i->event = e; }
void WINAPI ReleaseMutexWhenCallbackReturns(tp_instance_t *i, HANDLE m) { if (i) i->mutex = m; }
void WINAPI ReleaseSemaphoreWhenCallbackReturns(tp_instance_t *i, HANDLE s, DWORD n) {
    if (i) i->semaphore = s, i->sem_count = (LONG)n;
}
void WINAPI LeaveCriticalSectionWhenCallbackReturns(tp_instance_t *i, void *cs) { if (i) i->critsec = cs; }
void WINAPI FreeLibraryWhenCallbackReturns(tp_instance_t *i, HMODULE m) { if (i) i->library = m; }
BOOL WINAPI CallbackMayRunLong(tp_instance_t *i) { (void)i; return TRUE; }
void WINAPI DisassociateCurrentThreadFromCallback(tp_instance_t *i) { (void)i; }

/* ---- pools and cleanup groups ------------------------------------------------------------------------------ */

static int the_pool;
void *WINAPI CreateThreadpool(void *reserved) { (void)reserved; return &the_pool; }
void WINAPI CloseThreadpool(void *p) { (void)p; }
void WINAPI SetThreadpoolThreadMaximum(void *p, DWORD n) { (void)p, (void)n; }
BOOL WINAPI SetThreadpoolThreadMinimum(void *p, DWORD n) { (void)p, (void)n; return TRUE; }

void *WINAPI CreateThreadpoolCleanupGroup(void) {
    tp_group_t *g = calloc(1, sizeof(*g));
    if (g) pthread_mutex_init(&g->mu, 0);
    return g;
}

/* every member: its callbacks waited for (the queued ones dropped when
 * cancel), then closed */
void WINAPI CloseThreadpoolCleanupGroupMembers(tp_group_t *g, BOOL cancel, void *ctx) {
    (void)ctx;
    if (!g) return;
    for (;;) {
        tp_obj_t *o;
        pthread_mutex_lock(&g->mu);
        o = g->members;
        if (o) {
            g->members = o->group_next;
            o->group = 0;
        }
        pthread_mutex_unlock(&g->mu);
        if (!o) break;
        if (o->kind == TP_TIMER) WaitForThreadpoolTimerCallbacks(o, cancel);
        else if (o->kind == TP_WAIT) WaitForThreadpoolWaitCallbacks(o, cancel);
        else wait_callbacks(o, cancel);
        close_obj(o);
    }
}
void WINAPI CloseThreadpoolCleanupGroup(tp_group_t *g) {
    if (!g) return;
    pthread_mutex_destroy(&g->mu);
    free(g);
}

const wexport_t tpool_exports[] = {
    WEXP(CreateThreadpoolWork), WEXP(SubmitThreadpoolWork), WEXP(WaitForThreadpoolWorkCallbacks),
    WEXP(CloseThreadpoolWork), WEXP(TrySubmitThreadpoolCallback), WEXP(QueueUserWorkItem),
    WEXP(CreateThreadpoolTimer), WEXP(SetThreadpoolTimer), WEXPN("SetThreadpoolTimerEx", SetThreadpoolTimer),
    WEXP(IsThreadpoolTimerSet), WEXP(WaitForThreadpoolTimerCallbacks), WEXP(CloseThreadpoolTimer),
    WEXP(CreateThreadpoolWait), WEXP(SetThreadpoolWait), WEXPN("SetThreadpoolWaitEx", SetThreadpoolWait),
    WEXP(WaitForThreadpoolWaitCallbacks), WEXP(CloseThreadpoolWait), WEXP(SetEventWhenCallbackReturns),
    WEXP(ReleaseMutexWhenCallbackReturns), WEXP(ReleaseSemaphoreWhenCallbackReturns),
    WEXP(LeaveCriticalSectionWhenCallbackReturns), WEXP(FreeLibraryWhenCallbackReturns), WEXP(CallbackMayRunLong),
    WEXP(DisassociateCurrentThreadFromCallback), WEXP(CreateThreadpool), WEXP(CloseThreadpool),
    WEXP(SetThreadpoolThreadMaximum), WEXP(SetThreadpoolThreadMinimum), WEXP(CreateThreadpoolCleanupGroup),
    WEXP(CloseThreadpoolCleanupGroupMembers), WEXP(CloseThreadpoolCleanupGroup),
};
const int tpool_nexports = sizeof(tpool_exports) / sizeof(tpool_exports[0]);
