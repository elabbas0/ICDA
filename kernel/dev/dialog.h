#ifndef DEV_DIALOG_H
#define DEV_DIALOG_H

#include <stdint.h>

struct process;

uint64_t dialog_node_read(char *buf, uint64_t cap);
uint64_t dialog_node_write(const char *buf, uint64_t len);

/* The kernel's own dialog (a program that stopped, memory that ran out):
 * shown to uid's session (0: whoever is signed in); nobody waits for its
 * answer.  kind: "info", "warning" or "error"; app: the program's name. */
void dialog_post(const char *kind, const char *app, const char *title, const char *message, uint32_t uid);

/* a program that stopped (pid): the window manager closes its windows and
 * names it as they did, in place of label at the start of the title */
void dialog_post_stopped(uint64_t pid, const char *app, const char *label, const char *title,
                         const char *message, uint32_t uid);

/* A program ended: its request goes; the window manager's leaves the
 * requests for the next one. */
void dialog_proc_exit(struct process *proc);

/* "Editor" for "editor.app": the name a person knows a program by */
void dialog_app_label(const char *name, char *out, int cap);

#endif
