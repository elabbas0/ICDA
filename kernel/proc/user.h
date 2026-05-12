#ifndef USER_H
#define USER_H

#include <stdint.h>
#include "process.h"

#define USER_TEXT_BASE  0x0000008000400000ULL

int user_run_path(const char *path);
int user_spawn_path(const char *path, uint64_t *pid_out);
int user_wait_pid(uint64_t pid, uint64_t *exit_code_out);
int user_prepare_address_space(process_t *proc);
int user_map_blob(process_t *proc, const void *blob, uint64_t size, uint64_t virt_base);
void user_request_exit_to_kernel(uint64_t code);
uint64_t user_last_exit_code(void);
int user_run_demo(void);
void user_thread_start(void);
__attribute__((noreturn)) void user_thread_finish(void);
void user_enter(uint64_t rip, uint64_t rsp);

#endif
