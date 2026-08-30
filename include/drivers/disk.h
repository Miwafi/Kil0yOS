#ifndef DISK_H
#define DISK_H

#include "lib/types.h"

#define DISK_SECTOR_SIZE 512
/* 8 MB RAM disk: the embedded /bin payloads (~1.2 MB) plus TFTP-downloaded
 * packages must coexist (4096 sectors = 2 MB was too small for a second
 * busybox copy). */
#define DISK_MAX_SECTORS 16384

void disk_init();
int disk_read_sector(uint32_t sector, uint8_t* buffer);
int disk_write_sector(uint32_t sector, const uint8_t* buffer);

#endif