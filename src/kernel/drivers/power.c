#include <stdint.h>
#include "drivers/power.h"
#include "drivers/io.h"
#include "core/interrupts.h"
#include "lib/string.h"
#include "mm/memory.h"

/* RSDP (Root System Description Pointer) */
typedef struct {
    char     signature[8];
    uint8_t  checksum;
    uint8_t  oemid[6];
    uint8_t  revision;
    uint32_t rsdt_address;
} __attribute__((packed)) acpi_rsdp_t;

/* SDT header common to all ACPI tables */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

/* RSDT / XSDT entry count helper */
#define SDT_ENTRIES(ptr, is_xsdt) \
    (((ptr)->length - sizeof(acpi_sdt_header_t)) / ((is_xsdt) ? 8 : 4))

/* FADT - Fixed ACPI Description Table */
typedef struct {
    acpi_sdt_header_t h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  mon_alarm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved2;
    uint32_t flags;
} __attribute__((packed)) acpi_fadt_t;

static int acpi_available = 0;
static acpi_fadt_t* fadt = NULL;

/* Temporary mapping window for ACPI tables located above 4 GiB */
#define ACPI_TEMP_VIRT 0xFFFFFFFFC0000000ULL

static void* acpi_temp_map(uint64_t phys, size_t size) {
    if (phys < 0x100000000ULL) {
        return (void*)(uintptr_t)phys;
    }
    uint64_t start_page = phys & ~0xFFF;
    uint64_t offset = phys & 0xFFF;
    uint64_t pages = (offset + size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page(ACPI_TEMP_VIRT + i * PAGE_SIZE,
                     start_page + i * PAGE_SIZE,
                     VMM_PRESENT | VMM_WRITABLE);
    }
    return (void*)(ACPI_TEMP_VIRT + offset);
}

/* Simple checksum: sum of all bytes should be 0 */
static uint8_t checksum(void* ptr, size_t len) {
    uint8_t sum = 0;
    uint8_t* p = (uint8_t*)ptr;
    for (size_t i = 0; i < len; i++) {
        sum += p[i];
    }
    return sum;
}

/* Search for RSDP in EBDA and BIOS ROM area */
static acpi_rsdp_t* find_rsdp() {
    /* Check EBDA (first KB at 40:0E) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    uint16_t ebda_addr = *(volatile uint16_t*)(uintptr_t)0x0000040E;
#pragma GCC diagnostic pop
    if (ebda_addr) {
        ebda_addr *= 16; /* Convert to physical address */
        for (uint16_t off = 0; off < 1024; off += 16) {
            acpi_rsdp_t* rsdp = (acpi_rsdp_t*)(uintptr_t)(ebda_addr + off);
            if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0 &&
                checksum(rsdp, sizeof(acpi_rsdp_t)) == 0) {
                return rsdp;
            }
        }
    }

    /* Search BIOS ROM area (0xE0000 - 0xFFFFF) */
    for (uint32_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        acpi_rsdp_t* rsdp = (acpi_rsdp_t*)(uintptr_t)addr;
        if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0 &&
            checksum(rsdp, sizeof(acpi_rsdp_t)) == 0) {
            return rsdp;
        }
    }

    return NULL;
}

/* Find a table by signature in RSDT/XSDT */
static void* find_table(const char* sig) {
    acpi_rsdp_t* rsdp = find_rsdp();
    if (rsdp == NULL) return NULL;

    int is_xsdt = (rsdp->revision >= 2);

    if (!is_xsdt) {
        /* RSDT uses 32-bit pointers */
        acpi_sdt_header_t* rsdt = (acpi_sdt_header_t*)(uintptr_t)rsdp->rsdt_address;
        int entries = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4;
        uint32_t* ptrs = (uint32_t*)(rsdt + 1);
        for (int i = 0; i < entries; i++) {
            acpi_sdt_header_t* hdr = (acpi_sdt_header_t*)(uintptr_t)ptrs[i];
            if (memcmp(hdr->signature, sig, 4) == 0) return hdr;
        }
    } else {
        /* XSDT uses 64-bit pointers - read from extended field */
        uint64_t xsdt_addr = *(uint64_t*)((uint8_t*)rsdp + 24); /* offset 24 in v2+ RSDP */

        acpi_sdt_header_t* xsdt = (acpi_sdt_header_t*)acpi_temp_map(xsdt_addr, sizeof(acpi_sdt_header_t));
        int entries = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8;
        /* Remap with full entries region */
        xsdt = (acpi_sdt_header_t*)acpi_temp_map(xsdt_addr, sizeof(acpi_sdt_header_t) + entries * 8);
        uint64_t* ptrs = (uint64_t*)(xsdt + 1);
        for (int i = 0; i < entries; i++) {
            uint64_t entry_addr = ptrs[i];
            acpi_sdt_header_t* hdr = (acpi_sdt_header_t*)acpi_temp_map(entry_addr, sizeof(acpi_sdt_header_t));
            if (memcmp(hdr->signature, sig, 4) == 0) {
                uint32_t len = hdr->length;
                hdr = (acpi_sdt_header_t*)acpi_temp_map(entry_addr, len);
                /* Copy table to heap so the temp mapping can be reused safely */
                void* copy = kmalloc(len);
                if (copy) {
                    memcpy(copy, hdr, len);
                    return copy;
                }
                return hdr;
            }
        }
    }

    return NULL;
}

void* acpi_find_table(const char* sig) {
    return find_table(sig);
}

void power_init(void) {
    acpi_rsdp_t* rsdp = find_rsdp();
    if (rsdp != NULL) {
        fadt = (acpi_fadt_t*)find_table("FACP");
        if (fadt != NULL && checksum(fadt, fadt->h.length) == 0) {
            acpi_available = 1;
        }
    }
}

void power_shutdown(void) {
    /* Method 1: ACPI S5 shutdown via FADT PM1 control registers */
    if (acpi_available && fadt != NULL && fadt->pm1a_cnt_blk != 0) {
        /*
         * PM1_CNT register layout (16-bit):
         *   Bits [12:10] : SLP_TYP   (sleep type, 3 bits)
         *   Bit  [13]    : SLP_EN    (sleep enable)
         *
         * SLP_TYP values are OEM-specific (defined in DSDT AML).
         * Try all possible values since we lack an AML interpreter.
         */
        uint16_t pm1_port = fadt->pm1a_cnt_blk;

        /* Try each SLP_TYP value (0..7) combined with SLP_EN */
        for (int typ = 7; typ >= 0; typ--) {
            outw(pm1_port, ((uint16_t)typ << 10) | (1u << 13));

            /* Brief delay — if shutdown triggered, we never return */
            for (volatile int w = 0; w < 50000; w++) {
                __asm__ volatile("pause");
            }
        }

        /* Also try SLP_EN-only (some systems treat this as implicit S5) */
        outw(pm1_port, 1u << 13);
        for (volatile int w = 0; w < 50000; w++) {
            __asm__ volatile("pause");
        }

        /* Try PM1b_CNT as well if present */
        if (fadt->pm1b_cnt_blk != 0) {
            for (int typ = 7; typ >= 0; typ--) {
                outw(fadt->pm1b_cnt_blk, ((uint16_t)typ << 10) | (1u << 13));
                for (volatile int w = 0; w < 10000; w++)
                    __asm__ volatile("pause");
            }
        }
    }

    /* All ACPI attempts exhausted — halt to avoid triple fault */
    disable_interrupts();
    while (1) {
        __asm__ volatile("hlt");
    }
}
