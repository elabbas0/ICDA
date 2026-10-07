/* Threads on ICDA's Linux personality: build static with musl
 * (tests/linux/build.sh) and run it from the Terminal.  Prints PASS / FAIL
 * per check and "thtest: ALL PASS" at the end. */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (cond) printf("PASS %s\n", what); else { printf("FAIL %s\n", what); failures++; } fflush(stdout); } while (0)

/* 1: mutex-protected counter from many threads */
#define NT 8
#define ITER 20000
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static atomic_long acounter;
static __thread long tls_value;

static void *adder(void *arg) {
    long id = (long)arg;
    tls_value = id * 1000;
    for (int i = 0; i < ITER; i++) {
        pthread_mutex_lock(&mu);
        counter++;
        pthread_mutex_unlock(&mu);
        atomic_fetch_add(&acounter, 1);
        if ((i & 1023) == 0) sched_yield();
    }
    return (void *)(tls_value + id);   /* the thread's own TLS survived */
}

/* 2: producer / consumer with a condition variable */
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int queue[64], qn, produced_all;
static long consumed_sum;

static void *producer(void *arg) {
    (void)arg;
    for (int i = 1; i <= 1000; i++) {
        pthread_mutex_lock(&mu);
        while (qn == 64) pthread_cond_wait(&cv, &mu);
        queue[qn++] = i;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu);
    produced_all = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return 0;
}

static void *consumer(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (qn == 0 && !produced_all) pthread_cond_wait(&cv, &mu);
        if (qn == 0 && produced_all) {
            pthread_mutex_unlock(&mu);
            return 0;
        }
        consumed_sum += queue[--qn];
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mu);
    }
}

/* 4: detached threads finishing on their own */
static atomic_int detached_done;
static void *detached(void *arg) {
    (void)arg;
    usleep(20000);
    atomic_fetch_add(&detached_done, 1);
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    pthread_t th[NT];
    void *ret;
    int tls_ok = 1;
    double t0;

    printf("thtest: pid %d tid %ld\n", getpid(), (long)syscall(SYS_gettid));

    for (long i = 0; i < NT; i++) pthread_create(&th[i], 0, adder, (void *)i);
    for (long i = 0; i < NT; i++) {
        pthread_join(th[i], &ret);
        if ((long)ret != i * 1000 + i) tls_ok = 0;
    }
    CHECK(counter == (long)NT * ITER, "mutex counter");
    CHECK(atomic_load(&acounter) == (long)NT * ITER, "atomic counter");
    CHECK(tls_ok, "thread-local storage");

    {
        pthread_t p, c[3];
        pthread_create(&p, 0, producer, 0);
        for (int i = 0; i < 3; i++) pthread_create(&c[i], 0, consumer, 0);
        pthread_join(p, 0);
        for (int i = 0; i < 3; i++) pthread_join(c[i], 0);
        CHECK(consumed_sum == 1000L * 1001 / 2, "condition variable queue");
    }

    {   /* 3: a timed wait that times out */
        struct timespec ts;
        int r;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        t0 = now_s();
        pthread_mutex_lock(&mu);
        r = pthread_cond_timedwait(&cv, &mu, &ts);
        pthread_mutex_unlock(&mu);
        CHECK(r == ETIMEDOUT && now_s() - t0 > 0.15 && now_s() - t0 < 1.5, "timed wait");
    }

    {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        for (int i = 0; i < 4; i++) {
            pthread_t d;
            pthread_create(&d, &a, detached, 0);
        }
        t0 = now_s();
        while (atomic_load(&detached_done) < 4 && now_s() - t0 < 3) usleep(5000);
        CHECK(atomic_load(&detached_done) == 4, "detached threads");
    }

    {   /* 5: many short-lived threads (stacks and ids are recycled) */
        int ok = 1;
        for (int round = 0; round < 50; round++) {
            pthread_t t;
            if (pthread_create(&t, 0, detached, 0) != 0 || pthread_join(t, 0) != 0) { ok = 0; break; }
        }
        CHECK(ok, "create / join 50 times");
    }

    if (failures) printf("thtest: %d FAILED\n", failures);
    else printf("thtest: ALL PASS\n");
    return failures ? 1 : 0;
}
