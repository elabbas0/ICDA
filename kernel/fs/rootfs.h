#ifndef ROOTFS_H
#define ROOTFS_H

#include <stdint.h>
#include "../drivers/storage/partition.h"

/* "/" on ICDA's own partition (ICDA 1.9).
 *
 * Every file - the system's and the users' - is a file on that partition,
 * read when it is opened and freed from memory when it has not been used
 * for a while (kernel/fs/vfs.c, the file cache).  Only /dev, /tmp and
 * /volumes live in memory.  The boot files sit on the EFI system partition:
 *   \EFI\ICDA\GRUBX64.EFI  KERNEL.BIN  GRUBENV  and the previous kernel in
 *   \UPDATE\BACKUP\EFI\ICDA\KERNEL.BIN (where GRUB's menu looks for it).
 *
 * An installation from before 1.9 kept its system in \SYSTEM, its personal
 * files in the image \ICDAROOT.BIN loaded into memory, and the kernel on the
 * ICDA partition.  It is moved once (rootfs_boot), on the first start after
 * the update that brought 1.9 was confirmed healthy - until then the update
 * can still be rolled back to the earlier kernel, which needs the old layout. */

#define ROOTFS_LAYOUT_PATH "/etc/icda-layout"     /* on "/": "2" */

/* Finds ICDA's partition and moves an old layout once it is safe to.
 * 0: the 1.9 layout, to be mounted by rootfs_mount; -1: the earlier
 * in-memory system (a live start, an old layout not yet moved, no disk). */
int rootfs_boot(int live);
int rootfs_mount(void);                          /* then, after sysupdate_boot: "/" is the disk */

int rootfs_active(void);                          /* "/" is the disk */
int rootfs_new_layout(void);                      /* the 1.9 layout (known before the mount) */
const partition_info_t *rootfs_partition(void);   /* ICDA's partition */
const partition_info_t *rootfs_esp(void);         /* where the boot files are */
int rootfs_boot_on_esp(void);                     /* the kernel is on the EFI partition */

#endif
