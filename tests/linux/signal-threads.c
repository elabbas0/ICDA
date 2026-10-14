/* Per-thread masks and child-exit delivery to a blocked asynchronous worker. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static atomic_int stage, release_worker, reactor_done;
static int worker_inherited, worker_kept_mask, reactor_mask_restored;
static int notify_pipe[2], wait_word, reactor_rc, reactor_errno;
static volatile sig_atomic_t handler_tid, handler_blocked;
static int restart_pipe[2], restart_rc;
static char restart_byte;
static int passed, total;

static void pause_ms(int ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static void check(const char *name, int ok) {
    total++; passed += !!ok;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    fflush(stdout);
}
static int blocked(int sig) {
    sigset_t mask;
    pthread_sigmask(SIG_SETMASK, NULL, &mask);
    return sigismember(&mask, sig);
}
static void change(int sig, int how) {
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, sig);
    pthread_sigmask(how, &mask, NULL);
}
static void *mask_worker(void *unused) {
    (void)unused;
    worker_inherited = blocked(SIGUSR2);
    change(SIGUSR2, SIG_UNBLOCK);
    change(SIGUSR1, SIG_BLOCK);
    atomic_store(&stage, 1);
    while (!atomic_load(&release_worker)) pause_ms(1);
    worker_kept_mask = !blocked(SIGUSR2) && blocked(SIGUSR1);
    return NULL;
}
static void child_signal(int sig) {
    int saved = errno;
    char byte = 'C';
    (void)sig;
    handler_tid = (sig_atomic_t)syscall(SYS_gettid);
    /* sigprocmask and write are async-signal-safe. */
    sigset_t mask;
    sigprocmask(SIG_SETMASK, NULL, &mask);
    handler_blocked = sigismember(&mask, SIGCHLD);
    (void)write(notify_pipe[1], &byte, 1);
    errno = saved;
}
static void *reactor(void *unused) {
    (void)unused;
    change(SIGCHLD, SIG_UNBLOCK);
    atomic_store(&stage, 1);
    reactor_rc = (int)syscall(SYS_futex, &wait_word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    reactor_errno = errno;
    reactor_mask_restored = !blocked(SIGCHLD);
    atomic_store(&reactor_done, 1);
    return NULL;
}
static void restart_signal(int sig) {
    int saved = errno;
    char byte = 'S';
    (void)sig;
    (void)write(notify_pipe[1], &byte, 1);
    errno = saved;
}
static void *restart_reader(void *unused) {
    (void)unused;
    change(SIGUSR1, SIG_UNBLOCK);
    atomic_store(&stage, 1);
    restart_rc = (int)read(restart_pipe[0], &restart_byte, 1);
    return NULL;
}
int main(void) {
    pthread_t worker;
    sigset_t empty;
    sigemptyset(&empty); pthread_sigmask(SIG_SETMASK, &empty, NULL);
    change(SIGUSR2, SIG_BLOCK);
    if (pthread_create(&worker, NULL, mask_worker, NULL)) return 2;
    while (!atomic_load(&stage)) pause_ms(1);
    check("clone inherits creator mask", worker_inherited);
    check("worker mask changes preserve main mask", blocked(SIGUSR2) && !blocked(SIGUSR1));
    change(SIGUSR2, SIG_BLOCK);
    atomic_store(&release_worker, 1);
    pthread_join(worker, NULL);
    check("main mask changes preserve existing worker mask", worker_kept_mask);
    pid_t child = fork();
    if (!child) _exit(blocked(SIGUSR2) ? 17 : 18);
    int status = 0;
    check("fork inherits creator mask", child > 0 && waitpid(child, &status, 0) == child &&
          WIFEXITED(status) && WEXITSTATUS(status) == 17);

    struct sigaction sa = {0};
    sa.sa_handler = child_signal; sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    change(SIGCHLD, SIG_BLOCK);
    if (pipe2(notify_pipe, O_NONBLOCK | O_CLOEXEC)) return 2;
    atomic_store(&stage, 0);
    if (pthread_create(&worker, NULL, reactor, NULL)) return 2;
    while (!atomic_load(&stage)) pause_ms(1);
    check("worker unblocks SIGCHLD without changing main", blocked(SIGCHLD));
    pause_ms(50);  /* ensure the worker is asleep before the child exits */
    child = fork();
    if (!child) _exit(23);
    char byte = 0;
    for (int i = 0; i < 40 && read(notify_pipe[0], &byte, 1) != 1; i++) pause_ms(10);
    check("child exit wakes unblocked worker and delivers SIGCHLD", byte == 'C' &&
          handler_tid != syscall(SYS_gettid));
    /* Rescue a broken kernel so failures finish instead of hanging the suite. */
    for (int i = 0; i < 40 && !atomic_load(&reactor_done); i++) pause_ms(10);
    if (!atomic_load(&reactor_done)) syscall(SYS_futex, &wait_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    pthread_join(worker, NULL);
    check("signal interrupts the worker's indefinite futex", reactor_rc == -1 && reactor_errno == EINTR);
    check("handler blocks SIGCHLD locally and sigreturn restores mask", handler_blocked && reactor_mask_restored);
    status = 0;
    check("notified child can be reaped with its exit status", child > 0 &&
          waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 23);

    /* A different thread issues kill while this one is blocked in read.
     * SA_RESTART must restore read's number, not the global last syscall. */
    sa.sa_handler = restart_signal; sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    change(SIGUSR1, SIG_BLOCK);
    if (pipe(restart_pipe)) return 2;
    atomic_store(&stage, 0);
    if (pthread_create(&worker, NULL, restart_reader, NULL)) return 2;
    while (!atomic_load(&stage)) pause_ms(1);
    pause_ms(50);
    kill(getpid(), SIGUSR1);
    byte = 0;
    for (int i = 0; i < 40 && read(notify_pipe[0], &byte, 1) != 1; i++) pause_ms(10);
    check("SA_RESTART handler runs on the blocked reader", byte == 'S');
    (void)write(restart_pipe[1], "R", 1);
    /* Closing the writer also bounds a failed restart that re-enters read. */
    close(restart_pipe[1]);
    pthread_join(worker, NULL);
    check("SA_RESTART resumes this thread's read after another thread's kill", restart_rc == 1 && restart_byte == 'R');
    close(restart_pipe[0]);
    close(notify_pipe[0]); close(notify_pipe[1]);
    printf("signal threads: %d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
