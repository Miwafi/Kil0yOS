#ifndef SMP_H
#define SMP_H

#include "lib/types.h"

#define MAX_APS 16

extern volatile uint32_t cpu_usage_percent[MAX_APS + 1];

void smp_init(void);
uint32_t smp_get_cpu_count(void);
void smp_update_cpu_usage(void);

/* Local APIC register access (MMIO base from the MADT; available even on
 * single-CPU boots). Reg offsets are the standard LAPIC byte offsets. */
int      lapic_available(void);
uint32_t lapic_read_reg(uint32_t reg);
void     lapic_write_reg(uint32_t reg, uint32_t val);

#endif
