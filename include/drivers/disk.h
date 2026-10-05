#ifndef DISK_H
#define DISK_H

#include "lib/types.h"

#define DISK_SECTOR_SIZE 512
/* 32 MB RAM disk: embedded /bin payloads (~8 MB: busybox, musl/glibc
 * chains, art assets) plus Phase 4 package installs must coexist - real
 * Debian/Ubuntu libc6 extracts 13.4 MB (gconv modules included), the
 * .deb cache peaks at ~3.3 MB before cleanup (16 MB had no headroom). */
#define DISK_MAX_SECTORS 65536

void disk_init();
int disk_read_sector(uint32_t sector, uint8_t* buffer);
int disk_write_sector(uint32_t sector, const uint8_t* buffer);

/* RAM-disk Live-mode helpers (fs_init root selection). disk_use_ram_fallback
 * switches disk_read/write_sector onto an in-memory disk (no-op if already
 * active); disk_is_ram reports whether that backing is in place - the FAT
 * format path refuses to touch a real device while it is false. */
void disk_use_ram_fallback(void);
int disk_is_ram(void);

#endif