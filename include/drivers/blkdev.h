#ifndef BLKDEV_H
#define BLKDEV_H

#include "lib/types.h"

/* Minimal block device layer. Whole disks (hd0 = legacy ATA, sd0/sd1 =
 * AHCI SATA) and MBR partition wrappers (sd0p1...) register here; the
 * filesystem layer addresses them by name. */

typedef struct blkdev {
    const char* name;
    uint32_t sector_count;      /* total sectors for this device */
    int (*read)(struct blkdev* dev, uint32_t lba, uint32_t count, void* buf);
    int (*write)(struct blkdev* dev, uint32_t lba, uint32_t count, const void* buf);
    void* priv;
    struct blkdev* next;
} blkdev_t;

void blkdev_register(blkdev_t* dev);
blkdev_t* blkdev_find(const char* name);

/* Head of the registered-device list (the "mnt" command enumerates it by
 * walking ->next). */
blkdev_t* blkdev_list(void);

/* Multi-sector transfer; returns 0 on success, -1 on error. */
int blkdev_read(blkdev_t* dev, uint32_t lba, uint32_t count, void* buf);
int blkdev_write(blkdev_t* dev, uint32_t lba, uint32_t count, const void* buf);

/* Parse the MBR of `disk` and register a partition wrapper for the first
 * Linux (type 0x83) entry, named "<disk>p<slot>" (e.g. sd0p1). Returns the
 * partition device or NULL (no MBR signature / no Linux entry). */
blkdev_t* mbr_first_linux(blkdev_t* disk);

/* Geometry of a partition wrapper (kilinstall needs it for the MBR entry
 * and mkfs bounds). Returns 1 if `dev` is a partition, 0 otherwise. */
int blkdev_part_info(blkdev_t* dev, uint32_t* lba_start, uint32_t* lba_count);

#endif
