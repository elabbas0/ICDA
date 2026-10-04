#ifndef KERNEL_TTY_PTY_H
#define KERNEL_TTY_PTY_H

#include <stdint.h>

struct process;

#define PTY_MAX 8

int      pty_open(struct process *owner);
int      pty_owned_by(int id, const struct process *proc);
uint64_t pty_master_read(int id, char *buf, uint64_t cap);
uint64_t pty_master_write(int id, const char *buf, uint64_t len);
void     pty_set_size(int id, uint32_t cols, uint32_t rows);
void     pty_get_size(int id, uint32_t *cols, uint32_t *rows);
void     pty_slave_write(int id, const char *buf, uint64_t len);
int      pty_slave_read_char(int id);
int      pty_alive(int id);
void     pty_proc_exit(struct process *proc);

#endif
