#include "drivers/disk.h"
#include "drivers/io.h"
#include "drivers/device.h"
#include "lib/string.h"

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
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERROR 0x01

#define DISK_TIMEOUT_COUNT 100000

static int disk_present = 0;

static int disk_device_open(device_t* dev);
static int disk_device_close(device_t* dev);
static int disk_device_read(device_t* dev, void* buffer, size_t size);
static int disk_device_write(device_t* dev, const void* buffer, size_t size);
static int disk_device_ioctl(device_t* dev, int cmd, void* arg);

static device_t disk_device = {
    .name = "disk",
    .type = DEVICE_TYPE_DISK,
    .open = disk_device_open,
    .close = disk_device_close,
    .read = disk_device_read,
    .write = disk_device_write,
    .ioctl = disk_device_ioctl
};

static int disk_wait_ready() {
    uint8_t status;
    for (int i = 0; i < DISK_TIMEOUT_COUNT; i++) {
        status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        if (!(status & ATA_STATUS_BUSY)) return 0;
    }
    return -1;
}

static int disk_wait_drq() {
    uint8_t status;
    for (int i = 0; i < DISK_TIMEOUT_COUNT; i++) {
        status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
        if (status & ATA_STATUS_ERROR) return -1;
        if (status & ATA_STATUS_DRQ) return 0;
    }
    return -1;
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
    
    if (disk_wait_ready() != 0) {
        disk_present = 0;
        return;
    }

    status = inb(ATA_PRIMARY_IO_BASE + ATA_REG_STATUS);
    if ((status & ATA_STATUS_ERROR) || !(status & ATA_STATUS_DRQ)) {
        disk_present = 0;
        return;
    }
    
    for (int i = 0; i < 256; i++) {
        inw(ATA_PRIMARY_IO_BASE + ATA_REG_DATA);
    }
    
    disk_present = 1;
    device_register(&disk_device);
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

    if (disk_wait_ready() != 0 || disk_wait_drq() != 0) return -1;
    
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

    if (disk_wait_ready() != 0 || disk_wait_drq() != 0) return -1;
    
    for (int i = 0; i < 256; i++) {
        uint16_t word = (buffer[i * 2 + 1] << 8) | buffer[i * 2];
        outw(ATA_PRIMARY_IO_BASE + ATA_REG_DATA, word);
    }
    
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND, 0xE7);
    if (disk_wait_ready() != 0) return -1;
    
    return 0;
}

static int disk_device_open(device_t* dev) {
    return 0;
}

static int disk_device_close(device_t* dev) {
    return 0;
}

static int disk_device_read(device_t* dev, void* buffer, size_t size) {
    if (size == 0 || buffer == NULL) return -1;
    uint8_t* buf = (uint8_t*)buffer;
    uint32_t sector = 0;
    int offset = 0;
    
    while (offset < (int)size && sector < DISK_MAX_SECTORS) {
        uint8_t sector_buf[DISK_SECTOR_SIZE];
        if (disk_read_sector(sector, sector_buf) != 0) {
            return offset > 0 ? offset : -1;
        }
        
        int copy_size = DISK_SECTOR_SIZE;
        if (offset + copy_size > (int)size) {
            copy_size = size - offset;
        }
        
        memcpy(buf + offset, sector_buf, copy_size);
        offset += copy_size;
        sector++;
    }
    
    return offset;
}

static int disk_device_write(device_t* dev, const void* buffer, size_t size) {
    if (size == 0 || buffer == NULL) return -1;
    const uint8_t* buf = (const uint8_t*)buffer;
    uint32_t sector = 0;
    int offset = 0;
    
    while (offset < (int)size && sector < DISK_MAX_SECTORS) {
        uint8_t sector_buf[DISK_SECTOR_SIZE] = {0};
        
        int copy_size = DISK_SECTOR_SIZE;
        if (offset + copy_size > (int)size) {
            copy_size = size - offset;
        }
        
        memcpy(sector_buf, buf + offset, copy_size);
        
        if (disk_write_sector(sector, sector_buf) != 0) {
            return offset > 0 ? offset : -1;
        }
        
        offset += copy_size;
        sector++;
    }
    
    return offset;
}

static int disk_device_ioctl(device_t* dev, int cmd, void* arg) {
    return -1;
}