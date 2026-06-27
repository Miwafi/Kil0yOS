#ifndef SMP_H
#define SMP_H

#include "lib/types.h"

#define MAX_APS 16

extern volatile uint32_t cpu_usage_percent[MAX_APS + 1];

void smp_init(void);
uint32_t smp_get_cpu_count(void);
void smp_update_cpu_usage(void);

#endif
