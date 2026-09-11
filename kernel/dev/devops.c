#include "devops.h"

#define DEVOPS_MAX_ENTRIES 16

typedef struct {
    const char       *path;
    const dev_calls_t *calls;
} devops_entry_t;

static devops_entry_t devops_table[DEVOPS_MAX_ENTRIES];
static int devops_count = 0;

static int devops_streq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

int devops_register(const char *path, const dev_calls_t *calls) {
    int i;

    if (!path || !calls) {
        return -1;
    }
    for (i = 0; i < devops_count; i++) {
        if (devops_streq(devops_table[i].path, path)) {
            devops_table[i].calls = calls;
            return 0;
        }
    }
    if (devops_count >= DEVOPS_MAX_ENTRIES) {
        return -1;
    }
    devops_table[devops_count].path = path;
    devops_table[devops_count].calls = calls;
    devops_count++;
    return 0;
}

const dev_calls_t *devops_lookup(const char *path) {
    int i;

    if (!path) {
        return 0;
    }
    for (i = 0; i < devops_count; i++) {
        if (devops_streq(devops_table[i].path, path)) {
            return devops_table[i].calls;
        }
    }
    return 0;
}
