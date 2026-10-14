/* Exercise exit_group while sibling threads are actively using shared VM.
 * Run from a native test harness: each child must exit with status 37, and
 * the parent must remain alive to report every round. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 4
#define ROUNDS 24

static atomic_int ready;
static atomic_ulong shared_value;

static void *worker(void *unused) {
    struct timespec delay = { 0, 1000 };
    (void)unused;
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    for (;;) {
        for (int i = 0; i < 10000; i++)
            atomic_fetch_add_explicit(&shared_value, 1, memory_order_relaxed);
        (void)nanosleep(&delay, NULL);
    }
    return NULL;
}

static int child_case(void) {
    pthread_t threads[WORKERS];
    atomic_store(&ready, 0);
    atomic_store(&shared_value, 0);
    for (int i = 0; i < WORKERS; i++) {
        if (pthread_create(&threads[i], NULL, worker, NULL) != 0) return 2;
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) != WORKERS) {
        __asm__ volatile("pause" ::: "memory");
    }
    /* Exit as soon as every sibling is active in its shared-VM loop. */
    syscall(SYS_exit_group, 37);
    return 3;
}

int main(void) {
    int passed = 0;
    for (int i = 0; i < ROUNDS; i++) {
        pid_t pid = fork();
        if (pid == 0) _exit(child_case());
        if (pid < 0) break;
        int status = 0;
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 37) break;
        passed++;
    }
    printf("exit threads: %d/%d passed\n", passed, ROUNDS);
    return passed == ROUNDS ? 0 : 1;
}
