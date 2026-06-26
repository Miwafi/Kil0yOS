#include "drivers/disk.h"
#include "drivers/io.h"
#include "drivers/device.h"
#include "drivers/pci.h"
#include "mm/memory.h"
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
#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_WRITE_DMA 0xCA
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_STATUS_BUSY 0x80
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERROR 0x01

#define DISK_TIMEOUT_COUNT 100000

/* Bus Master IDE register offsets (primary channel) */
#define BM_COMMAND   0x00
#define BM_STATUS    0x02
#define BM_PRDT_ADDR 0x04

#define BM_CMD_START 0x01
#define BM_CMD_READ  0x08

#define BM_STATUS_ACTIVE 0x01
#define BM_STATUS_ERROR  0x02
#define BM_STATUS_INTR   0x04

static int disk_present = 0;
static int dma_enabled = 0;
static uint16_t bm_base = 0; /* Bus Master I/O base */

static uint8_t* dma_buffer = NULL;
static uint8_t* prdt_buffer = NULL;

typedef struct __attribute__((packed)) {
    uint32_t phys_addr;
    uint16_t byte_count;
    uint16_t flags;
} prd_entry_t;

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

static void ata_init_dma(void) {
    pci_device_t* ide = pci_find_class(0x01, 0x01);
    if (ide == NULL) return;

    uint32_t bar4 = pci_read_dword(ide->bus, ide->device, ide->function, 0x20);
    if (bar4 == 0 || bar4 == 0xFFFFFFFF) return;

    if (bar4 & 0x01) {
        bm_base = (uint16_t)(bar4 & ~0x03);
    } else {
        /* Memory-mapped BAR4 not supported in this simple driver */
        return;
    }

    dma_buffer = (uint8_t*)kmalloc(512 + sizeof(prd_entry_t) + 8);
    if (dma_buffer == NULL) return;

    /* Align PRDT to 8-byte boundary */
    uintptr_t prdt_raw = (uintptr_t)(dma_buffer + 512);
    prdt_raw = (prdt_raw + 7) & ~7;
    prdt_buffer = (uint8_t*)prdt_raw;

    /* Ensure buffer is below 4 GiB (identity mapped) */
    if ((uintptr_t)dma_buffer >= 0x100000000ULL ||
        (uintptr_t)prdt_buffer >= 0x100000000ULL) {
        dma_enabled = 0;
        return;
    }

    /* Stop any running DMA */
    outb(bm_base + BM_COMMAND, 0);
    uint8_t status = inb(bm_base + BM_STATUS);
    outb(bm_base + BM_STATUS, status | BM_STATUS_ERROR | BM_STATUS_INTR);

    dma_enabled = 1;
}

static int ata_do_dma(uint32_t sector, uint8_t* buffer, int is_write) {
    if (!dma_enabled || bm_base == 0) return -1;

    /* Copy data to DMA buffer for writes */
    if (is_write) {
        memcpy(dma_buffer, buffer, 512);
    }

    /* Build PRDT */
    prd_entry_t* prdt = (prd_entry_t*)prdt_buffer;
    prdt->phys_addr = (uint32_t)(uintptr_t)dma_buffer;
    prdt->byte_count = 512;
    prdt->flags = 0x8000; /* EOT */

    /* Stop DMA engine */
    outb(bm_base + BM_COMMAND, 0);

    /* Clear error/interrupt flags */
    uint8_t status = inb(bm_base + BM_STATUS);
    outb(bm_base + BM_STATUS, status | BM_STATUS_ERROR | BM_STATUS_INTR);

    /* Set PRDT address */
    uint32_t prdt_phys = (uint32_t)(uintptr_t)prdt_buffer;
    outd(bm_base + BM_PRDT_ADDR, prdt_phys);

    /* Select drive and LBA */
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_DEVICE, 0xE0 | ((sector >> 24) & 0x0F));
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_SECTOR_COUNT, 1);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_LOW, sector & 0xFF);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_MID, (sector >> 8) & 0xFF);
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_LBA_HIGH, (sector >> 16) & 0xFF);

    /* Send ATA command */
    outb(ATA_PRIMARY_IO_BASE + ATA_REG_COMMAND,
         is_write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA);

    /* Start DMA */
    uint8_t cmd = BM_CMD_START;
    if (is_write) cmd |= BM_CMD_READ;
    outb(bm_base + BM_COMMAND, cmd);

    /* Poll until DMA completes */
    for (int i = 0; i < DISK_TIMEOUT_COUNT * 10; i++) {
        status = inb(bm_base + BM_STATUS);
        if (!(status & BM_STATUS_ACTIVE)) break;
    }

    /* Clear interrupt */
    status = inb(bm_base + BM_STATUS);
    outb(bm_base + BM_STATUS, status | BM_STATUS_INTR);

    if (status & BM_STATUS_ERROR) {
        return -1;
    }

    /* Copy data from DMA buffer for reads */
    if (!is_write) {
        memcpy(buffer, dma_buffer, 512);
    }

    return 0;
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

    /* Try to enable Bus Master DMA */
    ata_init_dma();

    device_register(&disk_device);
}

int disk_read_sector(uint32_t sector, uint8_t* buffer) {
    if (!disk_present) return -1;
    if (sector >= DISK_MAX_SECTORS) return -1;
    if (buffer == NULL) return -1;

    /* Try DMA first, fall back to PIO */
    if (dma_enabled) {
        if (ata_do_dma(sector, buffer, 0) == 0) return 0;
    }

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

    /* Try DMA first, fall back to PIO */
    if (dma_enabled) {
        if (ata_do_dma(sector, (uint8_t*)buffer, 1) == 0) return 0;
    }

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
