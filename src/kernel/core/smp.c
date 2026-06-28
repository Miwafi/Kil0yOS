#include "core/smp.h"
#include "core/gdt.h"
#include "core/idt.h"
#include "core/interrupts.h"
#include "drivers/power.h"
#include "drivers/vga.h"
#include "drivers/io.h"
#include "mm/memory.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"

/* Embedded AP trampoline binary from objcopy */
extern char _binary_build_ap_trampoline_bin_start[];
extern char _binary_build_ap_trampoline_bin_end[];

#define TRAMPOLINE_ADDR      0x8000
#define TRAMPOLINE_PML4_OFF  8
#define TRAMPOLINE_STACK_OFF 16
#define TRAMPOLINE_ENTRY_OFF 24
#define AP_STACK_SIZE        32768

static volatile uint32_t* lapic_base = NULL;
static volatile uint32_t ap_ready_count = 0;
static uint8_t num_cpus = 1;
static uint8_t apic_ids[MAX_APS];

volatile uint32_t cpu_usage_percent[MAX_APS + 1] = {0};

/* MADT structures */
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
    uint32_t local_apic_addr;
    uint32_t flags;
    uint8_t  entries[];
} __attribute__((packed)) madt_t;

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_t;

typedef struct {
    uint8_t  type;
    uint8_t  length;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed)) madt_lapic_t;

static uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

static void lapic_send_ipi(uint8_t apic_id, uint32_t icr_low) {
    lapic_write(0x310, (uint32_t)apic_id << 24);
    lapic_write(0x300, icr_low);
    while (lapic_read(0x300) & (1u << 12));
}

static void smp_parse_madt(void) {
    madt_t* madt = (madt_t*)acpi_find_table("APIC");
    if (madt == NULL) return;

    lapic_base = (volatile uint32_t*)(uintptr_t)madt->local_apic_addr;
    if (lapic_base == NULL) {
        lapic_base = (volatile uint32_t*)0xFEE00000;
    }

    uint8_t* ptr = madt->entries;
    uint8_t* end = (uint8_t*)madt + madt->length;
    num_cpus = 0;

    while (ptr < end) {
        madt_entry_t* entry = (madt_entry_t*)ptr;
        if (entry->length == 0) break;

        if (entry->type == 0) {
            madt_lapic_t* lapic = (madt_lapic_t*)entry;
            if (lapic->flags & 0x01) {
                if (num_cpus < MAX_APS) {
                    apic_ids[num_cpus] = lapic->apic_id;
                    num_cpus++;
                }
            }
        }
        ptr += entry->length;
    }
}

static void smp_setup_trampoline(uint64_t stack, uint64_t entry) {
    uint64_t pml4_phys;
    __asm__ volatile("mov %%cr3, %0" : "=r"(pml4_phys));
    pml4_phys &= ~0xFFF;

    size_t tramp_size = (size_t)(_binary_build_ap_trampoline_bin_end - _binary_build_ap_trampoline_bin_start);
    memcpy((void*)TRAMPOLINE_ADDR, _binary_build_ap_trampoline_bin_start, tramp_size);

    *(uint32_t*)(TRAMPOLINE_ADDR + TRAMPOLINE_PML4_OFF)  = (uint32_t)pml4_phys;
    *(uint64_t*)(TRAMPOLINE_ADDR + TRAMPOLINE_STACK_OFF) = stack;
    *(uint64_t*)(TRAMPOLINE_ADDR + TRAMPOLINE_ENTRY_OFF) = entry;
}

void ap_main(void) {
    gdt_reload();
    idt_reload();
    __asm__ volatile("sti");

    __asm__ volatile("lock incl %0" : "+m"(ap_ready_count));

    while (1) {
        __asm__ volatile("hlt");
    }
}

void smp_init(void) {
    disable_interrupts();

    smp_parse_madt();
    if (num_cpus <= 1) {
        klog("SMP: no additional CPUs found\n");
        enable_interrupts();
        return;
    }

    char buf[16];
    klog("SMP: found ");
    itoa(num_cpus, buf, 10, sizeof(buf));
    vga_puts(buf);
    vga_puts(" CPU(s)\n");

    /* BSP is CPU 0 */
    uint8_t bsp_apic_id = (uint8_t)(lapic_read(0x20) >> 24);

    for (uint8_t i = 0; i < num_cpus; i++) {
        if (apic_ids[i] == bsp_apic_id) continue;

        klog("SMP: starting AP ");
        itoa(apic_ids[i], buf, 10, sizeof(buf));
        vga_puts(buf);
        vga_puts("... ");

        uint8_t* stack = (uint8_t*)kmalloc(AP_STACK_SIZE);
        if (stack == NULL) {
            vga_puts("no stack\n");
            continue;
        }

        uintptr_t stack_top = ((uintptr_t)stack + AP_STACK_SIZE) & ~0xF;
        smp_setup_trampoline(stack_top, (uint64_t)(uintptr_t)ap_main);

        uint32_t expected = ap_ready_count;

        /* Send INIT IPI */
        lapic_send_ipi(apic_ids[i], 0xC500);
        pit_delay_ms(10);

        /* Send SIPI */
        lapic_send_ipi(apic_ids[i], 0xC600 | (TRAMPOLINE_ADDR >> 12));
        pit_delay_ms(1);

        /* Send second SIPI */
        lapic_send_ipi(apic_ids[i], 0xC600 | (TRAMPOLINE_ADDR >> 12));

        /* Wait for AP to start (up to ~100ms) */
        int started = 0;
        for (int wait = 0; wait < 1000; wait++) {
            if (ap_ready_count > expected) {
                started = 1;
                break;
            }
            pit_delay_ms(1);
        }

        if (started) {
            vga_puts("ok\n");
        } else {
            vga_puts("timeout\n");
        }
    }

    char cpu_buf[16];
    klog("SMP: ");
    itoa(ap_ready_count + 1, cpu_buf, 10, sizeof(cpu_buf));
    vga_puts(cpu_buf);
    vga_puts(" CPU(s) online\n");

    enable_interrupts();
}

uint32_t smp_get_cpu_count(void) {
    return ap_ready_count + 1;
}

void smp_update_cpu_usage(void) {
    extern volatile uint64_t cpu_busy_ticks;
    extern volatile uint64_t cpu_idle_ticks;

    uint64_t busy = cpu_busy_ticks;
    uint64_t idle = cpu_idle_ticks;
    uint64_t total = busy + idle;

    if (total > 0) {
        cpu_usage_percent[0] = (uint32_t)((busy * 100) / total);
    } else {
        cpu_usage_percent[0] = 0;
    }

    /* AP cores are always idle (halt loop) */
    for (uint32_t i = 1; i < MAX_APS + 1; i++) {
        cpu_usage_percent[i] = 0;
    }

    cpu_busy_ticks = 0;
    cpu_idle_ticks = 0;
}
