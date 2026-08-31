#ifndef DISK_H
#define DISK_H

#include "lib/types.h"

#define DISK_SECTOR_SIZE 512
/* 32 MB RAM disk: embedded /bin payloads (~1.2 MB) plus Phase 4 package
 * installs must coexist - real Debian/Ubuntu libc6 extracts 13.4 MB (gconv
 * modules included), the .deb cache peaks at ~3.3 MB before cleanup
 * (16 MB had no headroom over busybox). */
#define DISK_MAX_SECTORS 65536

void disk_init();
int disk_read_sector(uint32_t sector, uint8_t* buffer);
int disk_write_sector(uint32_t sector, const uint8_t* buffer);

#endif