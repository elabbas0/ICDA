#ifndef IPC_MSGQ_H
#define IPC_MSGQ_H

#include <stdint.h>

#define MSGQ_MAX_QUEUES 32
#define MSGQ_MAX_MSGS   64
#define MSGQ_MSG_SIZE   64
#define MSGQ_NAME_MAX   64


uint64_t msgq_open(const char *name);


int msgq_send(uint64_t handle, const void *msg);



int msgq_recv(uint64_t handle, void *out, int block);


int msgq_poll(uint64_t handle);


void msgq_close(uint64_t handle);

#endif
