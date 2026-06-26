#ifndef PIT_H
#define PIT_H

#include "lib/types.h"

void pit_init(uint32_t frequency);
void pit_delay_ms(uint32_t ms);

#endif