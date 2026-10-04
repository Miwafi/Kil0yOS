#ifndef POWER_H
#define POWER_H

#include "lib/types.h"

/* Snapshot of the ACPI power-management state for the `power` shell
 * command. s5_source: 0 = none, 1 = parsed from the DSDT _S5 package,
 * 2 = brute-force fallback (shutdown scans SLP_TYP 0..7). */
typedef struct {
    int      acpi_available;
    int      fadt_revision;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    int      s5_valid;
    int      s5_source;
    uint8_t  s5_typa;
    uint8_t  s5_typb;
} power_info_t;

void power_init(void);
void power_shutdown(void);
void* acpi_find_table(const char* sig);
void power_get_info(power_info_t* out);

#endif
