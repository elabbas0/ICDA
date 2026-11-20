#ifndef SWAPAREA_H
#define SWAPAREA_H

/* Turns swap on at boot (swaparea.c): a swap partition, else /swapfile on
 * an installed system's partition.  live: a live start; forced_on/off:
 * icda.swap=on/off on the command line. */
void swap_area_boot(int live, int forced_on, int forced_off);

/* /etc/icda/swap: -1 off, 0 automatic, else the swap file's size in MB */
int  swap_area_wanted(void);

/* /swapfile on ICDA's partition (swapfile.c): wanted as above; 0 when swap
 * is on in it */
int  swapfile_boot(int wanted);
/* removes /swapfile, if there is one (swap is off, or uses a partition) */
void swapfile_remove(void);

#endif
