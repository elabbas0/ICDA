#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int word, finished;
static long long ns(struct timespec t) { return t.tv_sec * 1000000000LL + t.tv_nsec; }
static void *rescue(void *unused) {
    struct timespec start, now;
    (void)unused;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!__atomic_load_n(&finished, __ATOMIC_ACQUIRE)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ns(now)-ns(start)>=1000000000LL) break;
        usleep(1000);
    }
    syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, 0, 0, 0);
    return 0;
}
int main(void) {
    int serial = open("/dev/serial", O_WRONLY), fail=0;
    if (serial < 0) serial=STDOUT_FILENO;
    for(int clock=0; clock<2; clock++) {
        struct timespec deadline, start, end;
        pthread_t worker;
        __atomic_store_n(&finished, 0, __ATOMIC_RELEASE);
        if (pthread_create(&worker, 0, rescue, 0)) return 2;
        clock_gettime(clock ? CLOCK_REALTIME : CLOCK_MONOTONIC, &deadline);
        deadline.tv_nsec += 100000000;
        if(deadline.tv_nsec >= 1000000000) { deadline.tv_nsec -= 1000000000; deadline.tv_sec++; }
        clock_gettime(CLOCK_MONOTONIC, &start);
        int r=(int)syscall(SYS_futex, &word, FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG |
                          (clock ? FUTEX_CLOCK_REALTIME : 0), 0, &deadline, 0, FUTEX_BITSET_MATCH_ANY);
        int err=errno;
        clock_gettime(CLOCK_MONOTONIC, &end);
        __atomic_store_n(&finished, 1, __ATOMIC_RELEASE);
        pthread_join(worker, 0);
        long long elapsed=(ns(end)-ns(start))/1000000;
        int ok=r==-1 && err==ETIMEDOUT && elapsed>=80 && elapsed<250;
        fail += !ok;
        dprintf(serial, "futex-deadline: %s clock%d %lldms result%d errno%d\n", ok?"PASS":"FAIL", clock, elapsed,r,err);
        if(serial!=STDOUT_FILENO) printf("futex-deadline: %s clock%d %lldms result%d errno%d\n", ok?"PASS":"FAIL", clock, elapsed,r,err);
    }
    return fail?1:0;
}
