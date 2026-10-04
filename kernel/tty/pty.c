#include "pty.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../linux/lx.h"

#define PTY_IN_CAP  4096u
#define PTY_OUT_CAP 32768u

typedef struct {
    int      used;
    uint64_t owner_pid;
    uint32_t cols, rows;
    uint32_t in_head, in_tail;
    uint32_t out_head, out_tail;
    char     in[PTY_IN_CAP];
    char     out[PTY_OUT_CAP];
} pty_t;

static pty_t ptys[PTY_MAX];

static pty_t *pty_get(int id) {
    if (id < 1 || id > PTY_MAX || !ptys[id - 1].used) return 0;
    return &ptys[id - 1];
}

int pty_open(struct process *owner) {
    if (!owner) return -1;
    for (int i = 0; i < PTY_MAX; i++) {
        pty_t *p = &ptys[i];
        if (p->used) continue;
        p->used = 1;
        p->owner_pid = owner->pid;
        p->cols = 80;
        p->rows = 24;
        p->in_head = p->in_tail = 0;
        p->out_head = p->out_tail = 0;
        return i + 1;
    }
    return -1;
}

int pty_owned_by(int id, const struct process *proc) {
    pty_t *p = pty_get(id);
    return p && proc && p->owner_pid == proc->pid;
}

int pty_alive(int id) {
    return pty_get(id) != 0;
}

uint64_t pty_master_read(int id, char *buf, uint64_t cap) {
    pty_t *p = pty_get(id);
    uint64_t n = 0;
    if (!p) return 0;
    while (n < cap && p->out_tail != p->out_head) {
        buf[n++] = p->out[p->out_tail];
        p->out_tail = (p->out_tail + 1) % PTY_OUT_CAP;
    }
    return n;
}

static void pty_interrupt(pty_t *p) {
    process_t *victim = 0;
    for (uint64_t pid = 1; pid < 4096; pid++) {
        process_t *proc = sched_find_process(pid);
        if (!proc || proc->pty != (int)(p - ptys) + 1) continue;
        if (proc->state == PROCESS_EXITED || proc->state == PROCESS_REAPED) continue;
        if (!proc->parent || proc->parent->pty != proc->pty) continue;
        if (!victim || proc->pid > victim->pid) victim = proc;
    }
    if (victim && victim->linux_personality) lx_signal(victim, 2);
    else if (victim) sched_kill_process(victim->pid, 130);
}

uint64_t pty_master_write(int id, const char *buf, uint64_t len) {
    pty_t *p = pty_get(id);
    uint64_t n = 0;
    if (!p) return 0;
    for (; n < len; n++) {
        uint32_t next = (p->in_head + 1) % PTY_IN_CAP;
        if (buf[n] == 0x03) {
            pty_interrupt(p);
            continue;
        }
        if (next == p->in_tail) break;
        p->in[p->in_head] = buf[n];
        p->in_head = next;
    }
    if (n) sched_wake_input_waiters();
    return n;
}

void pty_set_size(int id, uint32_t cols, uint32_t rows) {
    pty_t *p = pty_get(id);
    if (!p || cols < 8 || rows < 2 || cols > 1000 || rows > 1000) return;
    p->cols = cols;
    p->rows = rows;
}

void pty_get_size(int id, uint32_t *cols, uint32_t *rows) {
    pty_t *p = pty_get(id);
    *cols = p ? p->cols : 80;
    *rows = p ? p->rows : 24;
}

void pty_slave_write(int id, const char *buf, uint64_t len) {
    pty_t *p = pty_get(id);
    for (uint64_t i = 0; p && i < len; i++) {
        uint32_t next = (p->out_head + 1) % PTY_OUT_CAP;
        int spins = 0;
        while (next == p->out_tail) {
            if (++spins > 2000 || !sched_find_process(p->owner_pid)) return;
            sched_sleep(1);
            p = pty_get(id);
            if (!p) return;
        }
        p->out[p->out_head] = buf[i];
        p->out_head = next;
    }
}

int pty_slave_read_char(int id) {
    pty_t *p = pty_get(id);
    char c;
    if (!p || p->in_tail == p->in_head) return -1;
    c = p->in[p->in_tail];
    p->in_tail = (p->in_tail + 1) % PTY_IN_CAP;
    return (int)(uint8_t)c;
}

void pty_proc_exit(struct process *proc) {
    if (!proc) return;
    for (int i = 0; i < PTY_MAX; i++) {
        pty_t *p = &ptys[i];
        if (!p->used || p->owner_pid != proc->pid) continue;
        p->used = 0;
        for (uint64_t pid = 1; pid < 4096; pid++) {
            process_t *child = sched_find_process(pid);
            if (child && child != proc && child->pty == i + 1 &&
                child->state != PROCESS_EXITED && child->state != PROCESS_REAPED) {
                child->pty = 0;
                sched_kill_process(pid, 129);
            }
        }
    }
}

int pty_slave_peek(int id) {
    pty_t *p = pty_get(id);
    return p && p->in_tail != p->in_head;
}
