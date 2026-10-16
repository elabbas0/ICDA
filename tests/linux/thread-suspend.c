/* The thread suspend/resume handshake WebKit's garbage collector uses
 * (WTF Thread::suspend on Linux): tgkill reaches the named thread only,
 * rt_sigsuspend waits with its own mask, and a handler with SA_RESTART
 * still ends the sigsuspend with EINTR. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static sem_t ack;
static atomic_int suspended, target_tid, handled_on, rounds_done, stop;
static volatile sig_atomic_t sigsuspend_rc, sigsuspend_errno;
static int passed, total;

static void check(const char *name, int ok) {
    total++; passed += !!ok;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    fflush(stdout);
}

static void on_usr1(int sig, siginfo_t *info, void *uc) {
    sigset_t wait_set;
    (void)sig; (void)info; (void)uc;
    if (atomic_load(&suspended)) return;            /* the resume signal */
    atomic_store(&handled_on, (int)syscall(SYS_gettid));
    atomic_store(&suspended, 1);
    sem_post(&ack);
    sigfillset(&wait_set);
    sigdelset(&wait_set, SIGUSR1);
    errno = 0;
    sigsuspend_rc = sigsuspend(&wait_set);
    sigsuspend_errno = errno;
    atomic_store(&suspended, 0);
    sem_post(&ack);
}

static void *worker(void *arg) {
    (void)arg;
    atomic_store(&target_tid, (int)syscall(SYS_gettid));
    while (!atomic_load(&stop)) {}                  /* busy, like a JS thread */
    return 0;
}

static void *bystander(void *arg) {
    (void)arg;
    while (!atomic_load(&stop)) usleep(100);        /* blocked in a call, unmasked */
    return 0;
}

int main(void) {
    struct sigaction sa = { 0 };
    pthread_t w, b[3];
    int wrong = 0, eintr = 1;
    sem_init(&ack, 0, 0);
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;          /* as WebKit installs it */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, 0);
    pthread_create(&w, 0, worker, 0);
    for (int i = 0; i < 3; i++) pthread_create(&b[i], 0, bystander, 0);
    while (!atomic_load(&target_tid)) {}
    for (int round = 0; round < 200; round++) {
        syscall(SYS_tgkill, getpid(), atomic_load(&target_tid), SIGUSR1);   /* suspend */
        sem_wait(&ack);
        if (atomic_load(&handled_on) != atomic_load(&target_tid)) wrong++;
        syscall(SYS_tgkill, getpid(), atomic_load(&target_tid), SIGUSR1);   /* resume */
        sem_wait(&ack);
        if (sigsuspend_rc != -1 || sigsuspend_errno != EINTR) eintr = 0;
        atomic_fetch_add(&rounds_done, 1);
    }
    atomic_store(&stop, 1);
    pthread_join(w, 0);
    for (int i = 0; i < 3; i++) pthread_join(b[i], 0);
    check("200 suspend/resume rounds completed", atomic_load(&rounds_done) == 200);
    check("tgkill handled on the named thread every time", wrong == 0);
    check("sigsuspend ended with EINTR despite SA_RESTART", eintr);
    printf("%d/%d thread-suspend checks passed\n", passed, total);
    return passed == total ? 0 : 1;
}
