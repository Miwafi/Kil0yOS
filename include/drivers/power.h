#ifndef POWER_H
#define POWER_H

#include "lib/types.h"

void power_init(void);
void power_shutdown(void);
void* acpi_find_table(const char* sig);

#endif
