#ifndef DISK_H
#define DISK_H

#include "types.h"

#define DISK_SECTOR_SIZE 512
#define DISK_MAX_SECTORS 4096

void disk_init();
int disk_read_sector(uint32_t sector, uint8_t* buffer);
int disk_write_sector(uint32_t sector, const uint8_t* buffer);

#endif