#ifndef PIT_H
#define PIT_H

#include "lib/types.h"

extern volatile uint64_t pit_ticks;

void pit_init(uint32_t frequency);
void pit_delay_ms(uint32_t ms);
void pit_format_time(char* buf, size_t len);

#endif