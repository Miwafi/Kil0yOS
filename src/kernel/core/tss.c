#include "core/tss.h"
#include "core/gdt.h"
#include "lib/string.h"

/* TSS structure */
static tss64_t tss;

/* TSS descriptor in GDT (requires two entries for 64-bit) */
extern gdt_entry_t gdt[5];
extern gdt_ptr_t gdt_ptr;

void tss_init(void) {
    /* Clear TSS */
    memset(&tss, 0, sizeof(tss));

    /* Set I/O Permission Bitmap offset (no IOPB) */
    tss.iopb_offset = sizeof(tss);

    /* Expand GDT to include TSS
     * Current GDT layout:
     * 0: Null
     * 1: Kernel Code (0x08)
     * 2: Kernel Data (0x10)
     * 3: User Code (0x18)
     * 4: User Data (0x20)
     * 5-6: TSS (0x28) - requires two entries for 64-bit TSS
     */

    /* We need to expand the GDT array - this is declared in gdt.c */
    /* For now, we'll use a fixed-size GDT and add TSS at index 5 */

    /* Set TSS base and limit using the GDT entry format */
    /* This will be done in gdt_init after we expand the GDT */
}

void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp[0] = stack;
}

void tss_load(void) {
    /* Load TR (Task Register) with TSS selector */
    /* TSS is at selector 0x28 (index 5, RPL=0, GDT) */
    __asm__ volatile("ltr %0" : : "r"((uint16_t)0x28));
}