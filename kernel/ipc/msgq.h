#ifndef IPC_MSGQ_H
#define IPC_MSGQ_H

#include <stdint.h>

#define MSGQ_MAX_QUEUES 32
#define MSGQ_MAX_MSGS   64
#define MSGQ_MSG_SIZE   64
#define MSGQ_NAME_MAX   64

/* Returns 1-based handle, 0 on failure */
uint64_t msgq_open(const char *name);

/* Enqueue a MSGQ_MSG_SIZE-byte message. Returns 0 ok, -1 full/bad */
int msgq_send(uint64_t handle, const void *msg);

/* Dequeue a message. block=0 non-blocking (returns -1 if empty).
   block=1 spins until a message arrives. Returns 0 ok. */
int msgq_recv(uint64_t handle, void *out, int block);

/* Returns 1 if message waiting, 0 if empty, -1 bad handle */
int msgq_poll(uint64_t handle);

/* Future ref-counting stub */
void msgq_close(uint64_t handle);

#endif
