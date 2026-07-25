#include "core/tss.h"
#include "core/gdt.h"
#include "lib/string.h"

/* TSS structure - must be accessible after GDT load */
static tss64_t tss;

void tss_init(void) {
    /* Clear TSS */
    memset(&tss, 0, sizeof(tss));

    /* Set I/O Permission Bitmap offset to end of TSS (no IOPB) */
    tss.iopb_offset = sizeof(tss);

    /* Set up TSS descriptor in GDT at index 5-6 (selector 0x28) */
    gdt_set_tss((uint64_t)&tss, sizeof(tss) - 1);

    /* Load the Task Register (TR) with TSS selector */
    tss_load();
}

void tss_set_kernel_stack(uint64_t stack) {
    tss.rsp[0] = stack;
}

void tss_load(void) {
    /* Load TR (Task Register) with TSS selector 0x28 */
    __asm__ volatile("ltr %0" : : "r"((uint16_t)TSS_SEL));
}