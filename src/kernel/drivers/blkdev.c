#include "drivers/blkdev.h"
#include "drivers/disk.h"
#include "drivers/video/vga.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "lib/stdlib.h"

static blkdev_t* devices = NULL;

void blkdev_register(blkdev_t* dev) {
    dev->next = devices;
    devices = dev;
}

blkdev_t* blkdev_find(const char* name) {
    for (blkdev_t* d = devices; d != NULL; d = d->next) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

blkdev_t* blkdev_list(void) {
    return devices;
}

int blkdev_read(blkdev_t* dev, uint32_t lba, uint32_t count, void* buf) {
    if (dev == NULL || dev->read == NULL || count == 0) return -1;
    if (lba >= dev->sector_count || count > dev->sector_count - lba) return -1;
    return dev->read(dev, lba, count, buf);
}

int blkdev_write(blkdev_t* dev, uint32_t lba, uint32_t count, const void* buf) {
    if (dev == NULL || dev->write == NULL || count == 0) return -1;
    if (lba >= dev->sector_count || count > dev->sector_count - lba) return -1;
    return dev->write(dev, lba, count, buf);
}

/* ---- MBR partition wrappers ------------------------------------------- */

typedef struct {
    blkdev_t* parent;
    uint32_t lba_start;
    uint32_t lba_count;
    char name[12];
} part_priv_t;

static int part_read(blkdev_t* dev, uint32_t lba, uint32_t count, void* buf) {
    part_priv_t* p = (part_priv_t*)dev->priv;
    if (lba >= p->lba_count || count > p->lba_count - lba) return -1;
    return blkdev_read(p->parent, p->lba_start + lba, count, buf);
}

static int part_write(blkdev_t* dev, uint32_t lba, uint32_t count, const void* buf) {
    part_priv_t* p = (part_priv_t*)dev->priv;
    if (lba >= p->lba_count || count > p->lba_count - lba) return -1;
    return blkdev_write(p->parent, p->lba_start + lba, count, buf);
}

int blkdev_part_info(blkdev_t* dev, uint32_t* lba_start, uint32_t* lba_count) {
    part_priv_t* p = (part_priv_t*)dev->priv;
    if (p->parent == NULL) return 0; /* not a partition wrapper */
    if (lba_start) *lba_start = p->lba_start;
    if (lba_count) *lba_count = p->lba_count;
    return 1;
}

blkdev_t* mbr_first_linux(blkdev_t* disk) {
    uint8_t sec[DISK_SECTOR_SIZE];
    if (blkdev_read(disk, 0, 1, sec) != 0) return NULL;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return NULL;

    for (int slot = 0; slot < 4; slot++) {
        const uint8_t* e = sec + 446 + slot * 16;
        if (e[4] != 0x83) continue;             /* Linux partition type */
        uint32_t start = *(const uint32_t*)(e + 8);
        uint32_t count = *(const uint32_t*)(e + 12);
        if (count == 0 || start < 2048) continue; /* implausible: would
            overlay the MBR/GRUB area - refuse rather than corrupt it */

        part_priv_t* p = (part_priv_t*)kmalloc(sizeof(part_priv_t));
        if (p == NULL) return NULL;
        p->parent = disk;
        p->lba_start = start;
        p->lba_count = count;
        strncpy(p->name, disk->name, sizeof(p->name) - 2);
        p->name[sizeof(p->name) - 2] = '\0';
        strcat(p->name, "p");
        {
            char n[4];
            itoa(slot + 1, n, 10, sizeof(n));
            strcat(p->name, n);
        }

        blkdev_t* dev = (blkdev_t*)kmalloc(sizeof(blkdev_t));
        if (dev == NULL) { kfree(p); return NULL; }
        dev->name = p->name;
        dev->sector_count = count;
        dev->read = part_read;
        dev->write = part_write;
        dev->priv = p;
        dev->next = NULL;
        blkdev_register(dev);
        klog("[blkdev] partition ");
        klog(p->name);
        klog(" at LBA ");
        char num[16];
        itoa((int)start, num, 10, sizeof(num));
        klog(num);
        klog(" (");
        itoa((int)count, num, 10, sizeof(num));
        klog(num);
        klog(" sectors)\n");
        return dev;
    }
    return NULL;
}
