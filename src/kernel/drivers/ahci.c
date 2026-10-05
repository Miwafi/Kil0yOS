#include "drivers/ahci.h"
#include "drivers/blkdev.h"
#include "drivers/pci.h"
#include "drivers/io.h"
#include "drivers/video/vga.h"
#include "mm/memory.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"

/* ---- register layout -------------------------------------------------- */

#define AHCI_GHC   0x04
#define AHCI_PI    0x0C
#define AHCI_PORT0 0x100
#define AHCI_PORT_STRIDE 0x80

/* per-port register offsets */
#define PxCLB  0x00
#define PxCLBU 0x04
#define PxFB   0x08
#define PxFBU  0x0C
#define PxIS   0x10
#define PxIE   0x14
#define PxCMD  0x18
#define PxTFD  0x20
#define PxSSTS 0x28
#define PxSIG  0x24
#define PxCI   0x38

#define GHC_AE (1u << 31)

#define PxCMD_ST  (1u << 0)
#define PxCMD_SUD (1u << 1)
#define PxCMD_POD (1u << 2)
#define PxCMD_FRE (1u << 4)
#define PxCMD_FR  (1u << 14)
#define PxCMD_CR  (1u << 15)

#define PxSSTS_DET_PRESENT 3

#define PxIS_TFES (1u << 30)

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC

/* ---- uncached window ---------------------------------------------------
 * Command list / FIS receive area / command table / the data buffer must
 * be RAM the controller AND the CPU agree on, so they live in one VMM_CD
 * window like the HDA rings: PHYSICAL addresses go to the controller,
 * the CPU touches the uncached alias. (hda.c discipline, see there.) */
#define AHCI_MMIO_VIRT 0xFFFFFFFFC2000000ULL
#define AWIN_MMIO 0x00000          /* BAR5 alias, 2 pages */
#define AWIN_CL   0x02000          /* command lists, 1 page/port */
#define AWIN_FB   0x04000          /* FIS receive areas, 1 page/port */
#define AWIN_CT   0x06000          /* command tables, 1 page/port */
#define AWIN_DATA 0x08000          /* shared 64 KiB transfer buffer */
#define AWIN_TOTAL 0x18000

#define AHCI_MAX_PORTS 2           /* sd0/sd1 */
#define XFER_SECTORS 128           /* per-command cap (single PRDT entry) */
#define XFER_BYTES (XFER_SECTORS * 512)

/* command list entry (32 bytes):
 * DW0 = CFL|W|... | PRDTL<<16, DW1 = PRDBC (byte count, device-written),
 * DW2/DW3 = 64-bit command table address. Getting CTBA's slot wrong makes
 * the controller read the table from address 0 and silently drop the
 * command (PxCI never clears). */
typedef struct __attribute__((packed)) {
    uint32_t flags_prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctba_upper;
    uint32_t reserved[4];
} ahci_cmdhdr_t;

/* PRDT entry (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t dba;
    uint32_t dba_upper;
    uint32_t reserved;
    uint32_t dbc;                  /* byte count - 1 in low 22 bits */
} ahci_prdt_t;

typedef struct {
    int present;
    uint32_t port_base;            /* MMIO offset of this port's registers */
    uint64_t phys;                 /* phys base of this port's page block */
    uint32_t clb_phys;
    uint32_t fb_phys;
    uint32_t ct_phys;
    uint64_t sectors;              /* capacity from IDENTIFY */
} ahci_port_t;

static volatile uint8_t* bar = NULL;      /* BAR5 alias (uncached) */
static ahci_port_t ports[AHCI_MAX_PORTS];
static uint8_t* data_win = NULL;          /* shared transfer buffer alias */
static uint32_t data_phys = 0;
static int ndisks = 0;

/* ---- MMIO helpers ------------------------------------------------------ */

static volatile uint32_t* preg(ahci_port_t* p, uint32_t off) {
    return (volatile uint32_t*)(uintptr_t)(bar + p->port_base + off);
}

static uint32_t pr32(ahci_port_t* p, uint32_t off) { return *preg(p, off); }
static void pw32(ahci_port_t* p, uint32_t off, uint32_t v) { *preg(p, off) = v; }

/* Double timeout: counter loop AND a wall-clock deadline. The bare counter
 * alone under-counts when VM exits stretch every MMIO access (the VMM-exit
 * polling lesson), the clock alone would spin forever if pit_ticks froze. */
static int wait_clear(volatile uint32_t* reg, uint32_t mask, uint64_t timeout_us) {
    uint64_t deadline = pit_uptime_us() + timeout_us;
    for (uint64_t i = 0; i < 200000000ULL; i++) {
        if ((*reg & mask) == 0) return 0;
        if (pit_uptime_us() > deadline) return -1;
        __asm__ volatile("pause");
    }
    return -1;
}

/* Same discipline, waiting for bits to SET (e.g. FR after enabling FRE). */
static int wait_set(volatile uint32_t* reg, uint32_t mask, uint64_t timeout_us) {
    uint64_t deadline = pit_uptime_us() + timeout_us;
    for (uint64_t i = 0; i < 200000000ULL; i++) {
        if ((*reg & mask) == mask) return 0;
        if (pit_uptime_us() > deadline) return -1;
        __asm__ volatile("pause");
    }
    return -1;
}

/* ---- command issue ------------------------------------------------------ */

static void build_prdt(ahci_prdt_t* prdt, uint32_t phys, uint32_t bytes) {
    prdt->dba = phys;
    prdt->dba_upper = 0;
    prdt->reserved = 0;
    prdt->dbc = (bytes - 1) & 0x3FFFFF;
}

/* Issue one DMA command in slot 0 and poll PxCI to completion.
 * is_write: 1 = host->device (DMA buffer already filled).
 * timeout_us: completion deadline (wall clock). */
static int ahci_do_cmd(ahci_port_t* p, uint8_t cmd, uint64_t lba,
                       uint16_t count, int is_write, uint64_t timeout_us) {
    ahci_cmdhdr_t* hdr = (ahci_cmdhdr_t*)(uintptr_t)(bar + AWIN_CL +
                          (uint32_t)(p - ports) * PAGE_SIZE);
    uint8_t* cfis = (uint8_t*)(uintptr_t)(bar + AWIN_CT +
                    (uint32_t)(p - ports) * PAGE_SIZE);
    ahci_prdt_t* prdt = (ahci_prdt_t*)(cfis + 128);

    memset(cfis, 0, 128);
    cfis[0] = 0x27;                /* host-to-device FIS */
    cfis[1] = 0x80;                /* C bit: update command register */
    cfis[2] = cmd;
    cfis[3] = 0;                   /* features */
    cfis[4] = (uint8_t)(lba & 0xFF);
    cfis[5] = (uint8_t)((lba >> 8) & 0xFF);
    cfis[6] = (uint8_t)((lba >> 16) & 0xFF);
    cfis[7] = 0x40;                /* LBA mode */
    cfis[8] = (uint8_t)((lba >> 24) & 0xFF);
    cfis[9] = (uint8_t)((lba >> 32) & 0xFF);
    cfis[10] = (uint8_t)((lba >> 40) & 0xFF);
    cfis[12] = (uint8_t)(count & 0xFF);
    cfis[13] = (uint8_t)(count >> 8);

    build_prdt(prdt, data_phys, (uint32_t)count * 512);

    /* CFL is the command FIS length in DWORDS minus 1: the 20-byte H2D
     * register FIS = 5 DW -> CFL = 4. Getting this wrong makes the
     * controller silently ignore the command (PxCI never clears). */
    hdr->flags_prdtl = 4u | (is_write ? 0x40u : 0u) | (1u << 16); /* CFL=4, W, PRDTL=1 */
    hdr->prdbc = 0;                /* transfer count, device-written */
    hdr->ctba = p->ct_phys;
    hdr->ctba_upper = 0;

    /* clear stale error/interrupt bits, then fire slot 0 */
    pw32(p, PxIS, 0xFFFFFFFF);
    pw32(p, PxCI, 1);

    if (wait_clear(preg(p, PxCI), 1, timeout_us) != 0) {
        klog("[ahci] timeout waiting for CI\n");
        pw32(p, PxCI, 1);          /* try to clear the stuck slot */
        return -1;
    }
    uint32_t is = pr32(p, PxIS);
    pw32(p, PxIS, is);             /* W1C */
    if (is & PxIS_TFES) {
        klog("[ahci] task file error\n");
        return -1;
    }
    return 0;
}

/* Chunked transfer through the shared uncached buffer. */
static int ahci_rw(ahci_port_t* p, uint64_t lba, uint32_t count,
                   uint8_t* buf, int is_write) {
    while (count > 0) {
        uint32_t n = count > XFER_SECTORS ? XFER_SECTORS : count;
        if (is_write) {
            memcpy(data_win, buf, n * 512);
        }
        if (ahci_do_cmd(p, is_write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                        lba, (uint16_t)n, is_write, 3000000) != 0) {
            return -1;
        }
        if (!is_write) {
            memcpy(buf, data_win, n * 512);
        }
        lba += n;
        count -= n;
        buf += n * 512;
    }
    return 0;
}

/* ---- blkdev glue -------------------------------------------------------- */

static int sd_read(blkdev_t* dev, uint32_t lba, uint32_t count, void* buf) {
    ahci_port_t* p = (ahci_port_t*)dev->priv;
    return ahci_rw(p, lba, count, (uint8_t*)buf, 0);
}

static int sd_write(blkdev_t* dev, uint32_t lba, uint32_t count, const void* buf) {
    ahci_port_t* p = (ahci_port_t*)dev->priv;
    return ahci_rw(p, lba, count, (uint8_t*)(uintptr_t)buf, 1);
}

/* ---- init --------------------------------------------------------------- */

static int port_start(ahci_port_t* p) {
    uint32_t cmd = pr32(p, PxCMD);
    if (cmd & PxCMD_ST) {
        cmd &= ~PxCMD_ST;
        pw32(p, PxCMD, cmd);
        if (wait_clear(preg(p, PxCMD), PxCMD_CR, 500000) != 0) return -1;
    }
    if (cmd & PxCMD_FRE) {
        cmd &= ~PxCMD_FRE;
        pw32(p, PxCMD, cmd);
        if (wait_clear(preg(p, PxCMD), PxCMD_FR, 500000) != 0) return -1;
    }
    return 0;
}

/* Issue IDENTIFY (0xEC) through the normal DMA path - on AHCI the ATA
 * PIO-in protocol is delivered as FISes anyway, so the uniform DMA route
 * is simpler and works for every ATA disk. */
static int port_identify(ahci_port_t* p, uint64_t* sectors) {
    /* short deadline: phantom ports (QEMU reports a signature on ports
     * with no drive) must not stall the boot for seconds each */
    if (ahci_do_cmd(p, ATA_CMD_IDENTIFY, 0, 1, 0, 1000000) != 0) return -1;
    /* words 100-103: LBA48 total sector count */
    uint32_t* id = (uint32_t*)data_win;
    uint64_t total = id[50];       /* word 100/101 */
    total |= (uint64_t)id[51] << 32; /* word 102/103 */
    if (total == 0) {
        /* LBA28 fallback: words 60/61 */
        total = id[30];            /* word 60/61 as one dword */
    }
    if (total == 0) return -1;
    *sectors = total;
    return 0;
}

void ahci_init(void) {
    /* fs_init (and thus disk_init -> ahci_init) runs BEFORE pci_init, so
     * the pci_find_class() device list is still empty here - probe the
     * config space directly. Real machines expose the SATA controller as
     * class 0106 (AHCI) or 0104 (RAID mode: the ABAR is still there);
     * class 0101 means IDE-compatibility mode in the firmware setup -
     * no ABAR exists and this driver cannot work there, so report it
     * clearly instead of failing silently. */
    uint32_t ahci_bus = 0, ahci_dev = 0, ahci_fn = 0;
    uint32_t ide_bus = 0, ide_dev = 0, ide_fn = 0;
    int found = 0, ide_found = 0;
    for (uint32_t b = 0; b < 16 && !found; b++) {
        for (uint32_t d = 0; d < 32 && !found; d++) {
            for (uint32_t f = 0; f < 8 && !found; f++) {
                if (pci_read_word(b, d, f, 0x00) == 0xFFFF) continue;
                if (pci_read_byte(b, d, f, 0x0B) != 0x01) continue;
                uint8_t sub = pci_read_byte(b, d, f, 0x0A);
                if (sub == 0x06 || sub == 0x04) {
                    ahci_bus = b; ahci_dev = d; ahci_fn = f;
                    found = 1;
                } else if (sub == 0x01 && !ide_found) {
                    ide_bus = b; ide_dev = d; ide_fn = f;
                    ide_found = 1;
                }
            }
        }
    }
    if (!found) {
        klog("[ahci] no AHCI/RAID controller found\n");
        if (ide_found) {
            char num[8];
            klog("[ahci] storage controller at ");
            itoa((int)ide_bus, num, 10, sizeof(num)); klog(num);
            klog(":"); itoa((int)ide_dev, num, 10, sizeof(num)); klog(num);
            klog("."); itoa((int)ide_fn, num, 10, sizeof(num)); klog(num);
            klog(" is in IDE compatibility mode (no ABAR) -");
            klog(" set SATA Mode to AHCI in the firmware setup\n");
        }
        return;
    }
    {
        char num[8];
        klog("[ahci] controller ");
        itoa((int)ahci_bus, num, 10, sizeof(num)); klog(num);
        klog(":"); itoa((int)ahci_dev, num, 10, sizeof(num)); klog(num);
        klog("."); itoa((int)ahci_fn, num, 10, sizeof(num)); klog(num);
        klog("\n");
    }

    uint32_t bar5 = pci_read_dword(ahci_bus, ahci_dev, ahci_fn, 0x24);
    if (bar5 == 0 || bar5 == 0xFFFFFFFF || (bar5 & 0x1)) {
        klog("[ahci] unusable BAR5\n");
        return;
    }
    uint64_t bar_phys = bar5 & ~0xFULL;
    if (((bar5 >> 1) & 0x3u) == 0x2u) {
        /* 64-bit MMIO BAR: real firmware may place the ABAR above 4 GiB */
        bar_phys |= (uint64_t)pci_read_dword(ahci_bus, ahci_dev, ahci_fn, 0x28)
                    << 32;
    }

    /* enable memory space + bus master */
    uint32_t cmd = pci_read_word(ahci_bus, ahci_dev, ahci_fn, 0x04);
    pci_write_word(ahci_bus, ahci_dev, ahci_fn, 0x04, (uint16_t)(cmd | 0x6));

    /* map the uncached window: BAR5 alias + per-port DMA structures */
    uint32_t cl_phys = pmm_alloc_pages(2);   /* 1 page/port command list */
    uint32_t fb_phys = pmm_alloc_pages(2);   /* 1 page/port FIS receive */
    uint32_t ct_phys = pmm_alloc_pages(2);   /* 1 page/port command table */
    uint32_t data = pmm_alloc_pages(AWIN_TOTAL / PAGE_SIZE - 8);
    if (cl_phys == 0 || fb_phys == 0 || ct_phys == 0 || data == 0) {
        klog("[ahci] DMA buffer allocation failed\n");
        return;
    }

    for (uint32_t off = 0; off < AWIN_TOTAL; off += PAGE_SIZE) {
        uint64_t phys;
        if (off < AWIN_CL) phys = bar_phys + off;
        else if (off < AWIN_FB) phys = cl_phys + (off - AWIN_CL);
        else if (off < AWIN_CT) phys = fb_phys + (off - AWIN_FB);
        else if (off < AWIN_DATA) phys = ct_phys + (off - AWIN_CT);
        else phys = data + (off - AWIN_DATA);
        vmm_map_page(AHCI_MMIO_VIRT + off, phys,
                     VMM_PRESENT | VMM_WRITABLE | VMM_CD);
    }

    bar = (volatile uint8_t*)(uintptr_t)AHCI_MMIO_VIRT;
    data_win = (uint8_t*)(uintptr_t)(AHCI_MMIO_VIRT + AWIN_DATA);
    data_phys = data;

    /* enable AHCI mode, interrupts off (pure polling) */
    uint32_t ghc = *(volatile uint32_t*)(uintptr_t)(bar + AHCI_GHC);
    *(volatile uint32_t*)(uintptr_t)(bar + AHCI_GHC) = (ghc | GHC_AE) & ~1u;

    uint32_t pi = *(volatile uint32_t*)(uintptr_t)(bar + AHCI_PI);

    for (int pidx = 0; pidx < 32 && ndisks < AHCI_MAX_PORTS; pidx++) {
        if (!(pi & (1u << pidx))) continue;

        ahci_port_t* p = &ports[ndisks];
        p->port_base = AHCI_PORT0 + pidx * AHCI_PORT_STRIDE;

        /* Real drives need spin-up time before the link establishes
         * (QEMU answers instantly, which is why this never mattered
         * there): power on + spin up, then grade the wait by what DET
         * reports - 3 = online, 1 = link coming up, 0 = recheck once
         * shortly before declaring the port empty. */
        pw32(p, PxIE, 0);
        pw32(p, PxCMD, pr32(p, PxCMD) | PxCMD_SUD | PxCMD_POD);
        uint32_t det = pr32(p, PxSSTS) & 0xF;
        if (det != PxSSTS_DET_PRESENT) {
            uint64_t wait_us = (det == 0x1) ? 2000000 : 300000;
            uint64_t deadline = pit_uptime_us() + wait_us;
            while (pit_uptime_us() < deadline) {
                det = pr32(p, PxSSTS) & 0xF;
                if (det == PxSSTS_DET_PRESENT) break;
                __asm__ volatile("pause");
            }
            if (det != PxSSTS_DET_PRESENT) continue;   /* empty port */
        }

        /* link is up - the device signature may lag briefly on real hw */
        uint32_t sig = 0xFFFFFFFF;
        {
            uint64_t deadline = pit_uptime_us() + 500000;
            while (pit_uptime_us() < deadline) {
                sig = pr32(p, PxSIG);
                if (sig != 0xFFFFFFFF) break;
                __asm__ volatile("pause");
            }
        }
        /* Skip ATAPI (0xEB140101, e.g. an IDE-mounted CDROM on q35)
         * outright: IDENTIFY is invalid for packet devices and would
         * only burn a timeout. */
        if (sig == 0 || sig == 0xFFFFFFFF || sig == 0xEB140101u) continue;

        if (port_start(p) != 0) {
            klog("[ahci] port start failed\n");
            continue;
        }

        /* program the DMA structures (ST/FRE are off now) */
        p->clb_phys = cl_phys + (uint32_t)ndisks * PAGE_SIZE;
        p->fb_phys = fb_phys + (uint32_t)ndisks * PAGE_SIZE;
        p->ct_phys = ct_phys + (uint32_t)ndisks * PAGE_SIZE;
        pw32(p, PxCLB, p->clb_phys);
        pw32(p, PxCLBU, 0);
        pw32(p, PxFB, p->fb_phys);
        pw32(p, PxFBU, 0);
        memset((void*)(uintptr_t)(bar + AWIN_CL + (uint32_t)ndisks * PAGE_SIZE),
               0, PAGE_SIZE);
        memset((void*)(uintptr_t)(bar + AWIN_FB + (uint32_t)ndisks * PAGE_SIZE),
               0, PAGE_SIZE);
        memset((void*)(uintptr_t)(bar + AWIN_CT + (uint32_t)ndisks * PAGE_SIZE),
               0, PAGE_SIZE);

        /* FIS receive enable, then command engine */
        pw32(p, PxCMD, pr32(p, PxCMD) | PxCMD_FRE | PxCMD_SUD | PxCMD_POD);
        if (wait_set(preg(p, PxCMD), PxCMD_FR, 500000) != 0) {
            klog("[ahci] FRE enable timeout\n");
            continue;
        }
        pw32(p, PxCMD, pr32(p, PxCMD) | PxCMD_ST);

        uint64_t total = 0;
        if (port_identify(p, &total) != 0) {
            klog("[ahci] identify failed on port ");
            char num[16];
            itoa(pidx, num, 10, sizeof(num));
            klog(num);
            klog("\n");
            continue;
        }
        p->present = 1;
        p->sectors = total;

        static blkdev_t sd_dev[AHCI_MAX_PORTS];
        static char sd_name[AHCI_MAX_PORTS][8];
        strcpy(sd_name[ndisks], "sd");
        {
            char n[4];
            itoa(ndisks, n, 10, sizeof(n));
            strcat(sd_name[ndisks], n);
        }
        sd_dev[ndisks].name = sd_name[ndisks];
        sd_dev[ndisks].sector_count = (uint32_t)(total > 0xFFFFFFFFULL
                                                 ? 0xFFFFFFFFULL : total);
        sd_dev[ndisks].read = sd_read;
        sd_dev[ndisks].write = sd_write;
        sd_dev[ndisks].priv = p;
        sd_dev[ndisks].next = NULL;
        blkdev_register(&sd_dev[ndisks]);

        klog("[ahci] ");
        klog(sd_name[ndisks]);
        klog(" registered: ");
        char num[24];
        itoa((int)(total >> 11), num, 10, sizeof(num)); /* MiB (2048 sectors) */
        klog(num);
        klog(" MiB\n");
        ndisks++;
    }

    if (ndisks == 0) {
        klog("[ahci] no disks detected\n");
    }
}
