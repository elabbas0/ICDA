#include <assert.h>
#include <stdio.h>
#include "../kernel/fs/volumes.c"
static partition_info_t parts[2];
static int active = -1;
int persistfs_active_partition(void) { return active; }
const partition_info_t *partition_get(uint32_t index) { return index < 2 ? &parts[index] : 0; }
static int writable(void *context, uint64_t lba, uint32_t count, const void *buf) {
    (void)context; (void)lba; (void)count; (void)buf; return 0;
}
int main(void) {
    block_device_t dev = {.write = writable};
    parts[0].device = parts[1].device = &dev;
    parts[0].role = PARTITION_ROLE_SYSTEM;
    parts[1].role = PARTITION_ROLE_SYSTEM;
    assert(!partition_writable(&parts[0], 1));
    active = 0;
    assert(partition_writable(&parts[0], 1));
    assert(!partition_writable(&parts[1], 1));
    assert(!partition_writable(&parts[0], 0));
    parts[0].role = PARTITION_ROLE_EFI;
    assert(!partition_writable(&parts[0], 1));
    parts[0].role = PARTITION_ROLE_DATA;
    assert(partition_writable(&parts[0], 1));
    dev.write = 0;
    assert(!partition_writable(&parts[0], 1));
    puts("Active ICDA system writes allowed; foreign system, EFI, read-only filesystem and device writes rejected");
    return 0;
}
