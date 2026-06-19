#include "disk.h"
#include "io.h"

#define ATA_PRIMARY_IO_BASE 0x1F0
#define ATA_SECONDARY_IO_BASE 0x170

#define ATA_REG_DATA 0x0
#define ATA_REG_ERROR 0x1
#define ATA_REG_FEATURES 0x1
#define ATA_REG_SECTOR_COUNT 0x2
#define ATA_REG_LBA_LOW 0x3
#define ATA_REG_LBA_MID 0x4
#define ATA_REG_LBA_HIGH 0x5
#define ATA_REG_DEVICE 0x6
#define ATA_REG_STATUS 0x7
#define ATA_REG_COMMAND 0x7

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_STATUS_BUSY 0x80
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_ERROR 0x01

static int disk_present = 0;

static void disk_wait_ready() {
    uint8_t status;
    do {
        status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
    } while (status & ATA_STATUS_BUSY);
}

static void disk_wait_drq() {
    uint8_t status;
    do {
        status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        if (status & ATA_STATUS_ERROR) return;
    } while (!(status & ATA_STATUS_DRQ));
}

void disk_init() {
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_DEVICE, 0xA0);
    io_wait();
    
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_SECTOR_COUNT, 0);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_LOW, 0);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_MID, 0);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_HIGH, 0);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();
    
    uint8_t status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
    if (status == 0) {
        disk_present = 0;
        return;
    }
    
    disk_wait_ready();
    
    status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
    if ((status & ATA_STATUS_ERROR) || !(status & ATA_STATUS_DRQ)) {
        disk_present = 0;
        return;
    }
    
    for (int i = 0; i < 256; i++) {
        inw(ATA_PRIMARY_IO_BASE + ATA_REG_DATA);
    }
    
    disk_present = 1;
}

int disk_read_sector(uint32_t sector, uint8_t* buffer) {
    if (!disk_present) return -1;
    if (sector >= DISK_MAX_SECTORS) return -1;
    if (buffer == NULL) return -1;
    
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_DEVICE, 0xE0 | ((sector >> 24) & 0x0F));
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_SECTOR_COUNT, 1);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_LOW, sector & 0xFF);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_MID, (sector >> 8) & 0xFF);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_HIGH, (sector >> 16) & 0xFF);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    
    disk_wait_ready();
    disk_wait_drq();
    
    for (int i = 0; i < 256; i++) {
        uint16_t word = inw(ATA_PRIMARY_IO_BASE + ATA_REG_DATA);
        buffer[i * 2] = word & 0xFF;
        buffer[i * 2 + 1] = (word >> 8) & 0xFF;
    }
    
    return 0;
}

int disk_write_sector(uint32_t sector, const uint8_t* buffer) {
    if (!disk_present) return -1;
    if (sector >= DISK_MAX_SECTORS) return -1;
    if (buffer == NULL) return -1;
    
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_DEVICE, 0xE0 | ((sector >> 24) & 0x0F));
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_SECTOR_COUNT, 1);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_LOW, sector & 0xFF);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_MID, (sector >> 8) & 0xFF);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_HIGH, (sector >> 16) & 0xFF);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    
    disk_wait_ready();
    disk_wait_drq();
    
    for (int i = 0; i < 256; i++) {
        uint16_t word = (buffer[i * 2 + 1] << 8) | buffer[i * 2];
        outw(ATA_PRIMARY_IO_BASE + ATA_REG_DATA, word);
    }
    
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, 0xE7);
    disk_wait_ready();
    
    return 0;
}