#ifndef NMI_WDT_H
#define NMI_WDT_H

#include "lib/types.h"

/* NMI watchdog: periodic APIC-timer NMI that samples the kernel heartbeat
 * in NMI context. Unlike the kwatchdog thread it keeps running while the
 * kernel main task is wedged in a cli'd loop (IRQ0 is dead there, NMIs
 * are not maskable). Panics when the heartbeat stays frozen for >10 s. */

/* Program the LAPIC timer for periodic NMI delivery (~100 ms). Gracefully
 * does nothing when no LAPIC was found (watchdog thread stays the sole
 * monitor in that case). */
void nmi_wdt_init(void);

/* Called from isr_handler on vector 2. Returns 1 if the NMI belonged to
 * the watchdog (EOI + sample done), 0 when not armed (let the generic
 * exception path report it). */
int nmi_wdt_claim(void);

/* Diagnostic: NMIs delivered so far since nmi_wdt_init(). */
uint32_t nmi_wdt_tick_count(void);

/* Diagnostic: 1 once the LAPIC timer NMI watchdog is armed. */
int nmi_wdt_armed(void);

#endif
