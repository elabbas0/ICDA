#include "msgq.h"
#include "../proc/sched.h"
#include <stdint.h>

typedef struct {
    int      valid;
    char     name[MSGQ_NAME_MAX];
    uint8_t  buffer[MSGQ_MAX_MSGS][MSGQ_MSG_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} msgq_t;

static msgq_t msgq_table[MSGQ_MAX_QUEUES];

static int msgq_name_eq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return (a[i] == 0 && b[i] == 0);
}

static void msgq_copy_name(char *dst, const char *src) {
    uint64_t i = 0;
    while (src[i] && i < MSGQ_NAME_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

uint64_t msgq_open(const char *name) {
    if (!name || !*name) return 0;

    for (uint64_t i = 0; i < MSGQ_MAX_QUEUES; i++) {
        if (msgq_table[i].valid && msgq_name_eq(msgq_table[i].name, name))
            return i + 1;
    }

    for (uint64_t i = 0; i < MSGQ_MAX_QUEUES; i++) {
        if (!msgq_table[i].valid) {
            msgq_table[i].valid = 1;
            msgq_table[i].head  = 0;
            msgq_table[i].tail  = 0;
            msgq_table[i].count = 0;
            msgq_copy_name(msgq_table[i].name, name);
            return i + 1;
        }
    }
    return 0;
}

int msgq_send(uint64_t handle, const void *msg) {
    if (!handle || handle > MSGQ_MAX_QUEUES || !msg) return -1;
    msgq_t *q = &msgq_table[handle - 1];
    if (!q->valid || q->count >= MSGQ_MAX_MSGS) return -1;

    uint8_t *slot = q->buffer[q->head];
    const uint8_t *src = (const uint8_t *)msg;
    for (int i = 0; i < MSGQ_MSG_SIZE; i++) slot[i] = src[i];

    q->head = (q->head + 1) % MSGQ_MAX_MSGS;
    q->count++;
    return 0;
}

int msgq_recv(uint64_t handle, void *out, int block) {
    if (!handle || handle > MSGQ_MAX_QUEUES || !out) return -1;
    msgq_t *q = &msgq_table[handle - 1];
    if (!q->valid) return -1;

    if (block) {
        uint64_t spin = 0;
        while (q->count == 0) {
            /* Sleep a tick between checks so a blocking reader does not
             * busy-spin the whole scheduler at 100Hz. */
            sched_sleep(1);
            spin++;
            if (spin > 10000000ULL) {
                /* hard limit to avoid infinite hang */
                return -1;
            }
        }
    }

    if (q->count == 0) return -1;

    uint8_t *slot = q->buffer[q->tail];
    uint8_t *dst  = (uint8_t *)out;
    for (int i = 0; i < MSGQ_MSG_SIZE; i++) dst[i] = slot[i];

    q->tail = (q->tail + 1) % MSGQ_MAX_MSGS;
    q->count--;
    return 0;
}

int msgq_poll(uint64_t handle) {
    if (!handle || handle > MSGQ_MAX_QUEUES) return -1;
    msgq_t *q = &msgq_table[handle - 1];
    if (!q->valid) return -1;
    return (q->count > 0) ? 1 : 0;
}

void msgq_close(uint64_t handle) {
    (void)handle;
}
