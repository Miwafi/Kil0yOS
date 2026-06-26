#ifndef SMP_H
#define SMP_H

#include "lib/types.h"

#define MAX_APS 16

void smp_init(void);
uint32_t smp_get_cpu_count(void);

#endif
