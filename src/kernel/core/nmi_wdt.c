#include "core/nmi_wdt.h"
#include "core/smp.h"
#include "core/process.h"
#include "sched/scheduler.h"
#include "timer/pit.h"
#include "mm/memory.h"
#include "drivers/video/vga.h"
#include "lib/string.h"
#include "lib/stdlib.h"

/* =====================  NMI watchdog (APIC timer)  ======================
 *
 * The kwatchdog thread cannot observe a kernel main task wedged inside a
 * cli'd loop: cli freezes IRQ0, IRQ0 freezes the scheduler, the thread
 * never runs again. The NMI line has no mask - this watchdog programs the
 * local APIC timer for PERIODIC NMI DELIVERY (LVT timer, delivery mode
 * 100b) so a check runs every ~100 ms no matter what IF=0 code is doing:
 *
 *   - sample kernel_heartbeat every 10 s (TSC clock, IF-independent)
 *   - skip while a user process is active (main task legitimately parked)
 *   - frozen heartbeat with no user process -> panic() from NMI context
 *
 * The APIC timer is core-local and mandatory in long mode, so this works
 * on QEMU, VMware and real hardware alike - no PMU/PERFEVTSEL needed.
 * LAPIC base comes from the MADT (smp.c); the APIC is force-enabled via
 * MSR IA32_APIC_BASE.AE + the spurious-interrupt register. If any of
 * that is missing, init bails out and the kwatchdog thread remains the
 * only monitor (window: cli-wedges go unseen, as before). */

#define MSR_IA32_APIC_BASE 0x1B

#define LAPIC_EOI        0x0B0
#define LAPIC_SPURIOUS   0x0F0
#define LAPIC_LVT_TIMER  0x320
#define LAPIC_DIV_CONF   0x3E0
#define LAPIC_INIT_CNT   0x380
#define LAPIC_CUR_CNT    0x390

#define LVT_TIMER_MASKED     (1u << 16)
#define LVT_TIMER_PERIODIC   (1u << 17)
#define LVT_DELIVERY_NMI     (4u << 8)
#define NMI_VECTOR           2

#define NMI_WDT_PERIOD_US    100000ULL          /* ~100 ms                  */
#define NMI_WDT_TIMEOUT_US   (10ULL * 1000000ULL) /* 10 s, same as thread  */
#define NMI_WDT_CALIB_US     50000ULL           /* 50 ms calibration window */

static volatile int      nmi_armed = 0;
static volatile uint32_t nmi_ticks  = 0;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" : : "a"((uint32_t)val), "d"((uint32_t)(val >> 32)), "c"(msr));
}

/* The sampling state lives here, not in the thread: NMI context has no
 * stack to park a "deadline" loop on - each delivery re-evaluates from
 * static state. Volatile-safe against the interrupted context. */
static uint64_t nmi_last_seen   = 0;
static uint64_t nmi_window_open = 0;
static int      nmi_first       = 1;

static void nmi_wdt_sample(void) {
    uint64_t now = pit_uptime_us();

    if (nmi_first) {
        nmi_first       = 0;
        nmi_last_seen   = kernel_heartbeat_read();
        nmi_window_open = now;
        return;
    }
    if (now - nmi_window_open < NMI_WDT_TIMEOUT_US) return;
    nmi_window_open = now;

    /* A user process owning the CPU time means kernel main is parked in
     * wait4 / the shell hlt-wait on purpose - not a hang (same policy as
     * the kwatchdog thread). */
    if (process_any_active()) return;

    uint64_t hb = kernel_heartbeat_read();
    if (hb == nmi_last_seen) {
        char msg[96];
        char num[16];
        strcpy(msg, "NMI watchdog: heartbeat frozen >10s - cli wedge? (count=");
        utoa((uint32_t)hb, num, 10, sizeof(num));
        strcat(msg, num);
        strcat(msg, ", uptime=");
        utoa((uint32_t)(now / 1000000ULL), num, 10, sizeof(num));
        strcat(msg, num);
        strcat(msg, "s)");
        PANIC(msg);
    }
    nmi_last_seen = hb;
}

void nmi_wdt_init(void) {
    if (!lapic_available()) {
        klog("nmi_wdt: no LAPIC - thread watchdog only\n");
        return;
    }

    /* APIC global enable (MSR IA32_APIC_BASE bit 11): BIOS/firmware leave
     * it set on essentially every x86-64 boot, but force it anyway - MMIO
     * behaviour is UNDEFINED while it is clear. */
    uint64_t apic_msr = rdmsr(MSR_IA32_APIC_BASE);
    if (!(apic_msr & (1ULL << 11))) {
        wrmsr(MSR_IA32_APIC_BASE, apic_msr | (1ULL << 11));
        if (!(rdmsr(MSR_IA32_APIC_BASE) & (1ULL << 11))) {
            klog("nmi_wdt: APIC global-enable failed - thread watchdog only\n");
            return;
        }
    }

    /* Software enable via the spurious-interrupt register (keeps the
     * firmware spurious vector; we send no IPIs so spurious IRQs do not
     * arise in practice). */
    uint32_t svr = lapic_read_reg(LAPIC_SPURIOUS);
    if (!(svr & 0x100)) {
        lapic_write_reg(LAPIC_SPURIOUS, svr | 0x100);
    }

    /* Calibrate the APIC timer against the TSC clock: free-run from
     * 0xFFFFFFFF for 50 ms, count the ticks that fell out. */
    lapic_write_reg(LAPIC_LVT_TIMER, LVT_TIMER_MASKED);
    lapic_write_reg(LAPIC_DIV_CONF, 3);          /* divide by 16 */
    lapic_write_reg(LAPIC_INIT_CNT, 0xFFFFFFFFu);

    uint64_t t0 = pit_uptime_us();
    while (pit_uptime_us() - t0 < NMI_WDT_CALIB_US) { }
    uint32_t fell = 0xFFFFFFFFu - lapic_read_reg(LAPIC_CUR_CNT);

    uint32_t init = (uint32_t)((uint64_t)fell * NMI_WDT_PERIOD_US / NMI_WDT_CALIB_US);
    if (fell == 0 || init == 0) {
        klog("nmi_wdt: timer did not count - thread watchdog only\n");
        lapic_write_reg(LAPIC_LVT_TIMER, LVT_TIMER_MASKED);
        return;
    }

    /* Arm: periodic, NMI delivery, vector 2 (matches IDT gate 2). */
    lapic_write_reg(LAPIC_INIT_CNT, init);
    lapic_write_reg(LAPIC_LVT_TIMER, LVT_DELIVERY_NMI | NMI_VECTOR | LVT_TIMER_PERIODIC);

    nmi_armed = 1;
    klog("nmi_wdt: armed (APIC timer NMI, ~");
    char buf[16];
    utoa((uint32_t)(NMI_WDT_PERIOD_US / 1000), buf, 10, sizeof(buf));
    klog(buf);
    klog("ms period, init=");
    utoa(init, buf, 10, sizeof(buf));
    klog(buf);
    klog(")\n");
}

int nmi_wdt_claim(void) {
    if (!nmi_armed) return 0;

    /* A panic is printing (maybe the one we triggered): EOI and get out
     * so the panic output is not interrupted mid-line. */
    if (panic_in_progress()) {
        lapic_write_reg(LAPIC_EOI, 0);
        return 1;
    }

    lapic_write_reg(LAPIC_EOI, 0);   /* LAPIC needs an EOI even for NMI */
    nmi_ticks++;
    nmi_wdt_sample();
    return 1;
}

uint32_t nmi_wdt_tick_count(void) {
    return nmi_ticks;
}

int nmi_wdt_armed(void) {
    return nmi_armed;
}
