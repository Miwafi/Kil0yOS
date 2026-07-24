#ifndef TSS_H
#define TSS_H

#include "lib/types.h"

/* Task State Segment (TSS) for x86-64 */
typedef struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;           /* Stack pointer for privilege level 0 */
    uint64_t rsp1;           /* Stack pointer for privilege level 1 */
    uint64_t rsp2;           /* Stack pointer for privilege level 2 */
    uint64_t reserved1;
    uint64_t ist[7];         /* Interrupt Stack Table pointers */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;    /* I/O Permission Bitmap offset */
} __attribute__((packed)) tss_entry_t;

/* TSS with 64-byte alignment for proper GDT access */
typedef struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];         /* rsp0, rsp1, rsp2 */
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss64_t;

void tss_init(void);
void tss_set_kernel_stack(uint64_t stack);
void tss_load(void);

#endif