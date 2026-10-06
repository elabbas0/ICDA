#ifndef SYSUPDATE_H
#define SYSUPDATE_H

#include <stdint.h>

/* System files and over-the-air patches on the ICDA system partition.
 *
 *   \SYSTEM\...            system files, loaded into the VFS at boot
 *                          (\SYSTEM\apps\wm.app -> /apps/wm.app)
 *   \EFI\ICDA\KERNEL.BIN   kernel, \EFI\ICDA\GRUBX64.EFI boot loader
 *   \EFI\ICDA\GRUBENV      GRUB environment: icda_trial for rollback
 *   \UPDATE\STAGE\...      downloaded files of the pending patch
 *   \UPDATE\PENDING.TXT    the pending patch (applied at restart)
 *   \UPDATE\BACKUP\...     files the last patch replaced, LIST.TXT says how
 *   \UPDATE\STATE.TXT      versions that failed and were rolled back
 *
 * A patch only ever touches \SYSTEM, \EFI\ICDA and \UPDATE: personal files
 * (the ICDAROOT.BIN overlay) are never read or written here. */

/* Boot: finish or undo a patch according to GRUBENV, then load \SYSTEM. */
void sysupdate_boot(void);

/* Restart/shutdown: install the pending patch, if any. */
void sysupdate_apply(void);

/* /dev/sysupdate */
uint64_t sysupdate_node_read(char *buf, uint64_t cap);
uint64_t sysupdate_node_write(const char *buf, uint64_t len);   /* kernel buffer, <= 256 */
uint64_t sysupdate_write_user(const char *buf, uint64_t len);   /* large writes ("put") */

#endif
