#include "timer/pit.h"
#include "drivers/io.h"
#include "core/interrupts.h"

#define PIT_COMMAND   0x43
#define PIT_CHANNEL0  0x40
#define PIT_CHANNEL2  0x42
#define PIT_BASE_FREQ 1193180

static uint32_t pit_divisor = 1193;

volatile uint64_t pit_ticks = 0;

static uint16_t pit_read_counter(void);
static uint64_t pit_poll_elapsed_us(void);
static void tsc_calibrate(void);

/* Uptime in microseconds, TSC-based.
 *
 * Why not PIT ticks: the old dual-regime design (tick count + in-period
 * interpolation) had two failure modes, both observed on VMware:
 *  - Sampling race: pit_ticks was read AFTER the countdown register, so a
 *    tick landing between the two reads made uptime overshoot by ~one
 *    period and snap back on the next call - the ~9-10ms BACKWARD jumps
 *    visible throughout the boot log.
 *  - Frozen ticks: with IF=0 (the whole boot until enable_interrupts)
 *    IRQ0 never fires, pit_ticks stays at its last value and the
 *    interpolated component sawtooths inside a 10ms window. dhcp_wait's
 *    4s deadline then became unreachable - the first cold boot froze
 *    forever inside the DHCP wait with zero further output.
 * The TSC is monotonic, needs no interrupts, and has no sampling race.
 * It is calibrated once against the polled PIT countdown (which only
 * serves as the fallback if calibration fails). x86-64 guarantees RDTSC. */
static uint64_t tsc_base   = 0;  /* TSC value at pit_init()          */
static uint64_t tsc_per_us = 0;  /* calibrated TSC counts per us     */
static int      tsc_ready  = 0;

static inline uint64_t pit_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t pit_uptime_us(void) {
    if (tsc_ready)
        return (pit_rdtsc() - tsc_base) / tsc_per_us;
    return pit_poll_elapsed_us();
}

/* Elapsed microseconds since pit_init(). Pure polling of the PIT countdown
 * register with a soft wrap detector - works with IF=0 (before
 * enable_interrupts()), where IRQ0 never fires and pit_ticks stays 0.
 * Resolution: one wrap detection per call, so intervals longer than one
 * PIT cycle (10 ms at 100 Hz) may under-count by a few ms. */
static uint64_t pit_poll_elapsed_us(void) {
    static uint16_t last_progress = 0;
    static uint64_t total_counts = 0;

    uint64_t flags;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags));
    uint16_t cur = pit_read_counter();
    uint16_t progress = (uint16_t)(pit_divisor - cur);

    if (progress >= last_progress) {
        total_counts += progress - last_progress;
    } else {
        /* counter wrapped since the last call */
        total_counts += progress + ((uint32_t)pit_divisor - last_progress);
    }
    last_progress = progress;
    uint64_t counts = total_counts;
    __asm__ volatile("pushq %0\n\tpopfq" :: "r"(flags));

    return counts * 1000000ULL / PIT_BASE_FREQ;
}

void pit_format_time(char* buf, size_t len) {
    if (len < 24) { buf[0] = '\0'; return; }

    uint64_t usec = pit_uptime_us();
    uint64_t sec = usec / 1000000ULL;
    uint64_t frac = usec % 1000000ULL;

    /* Cap seconds to avoid buffer overflow (max ~136 years at 100Hz) */
    if (sec > 99999) sec = 99999;

    char sec_str[8];
    int sec_len = 0;
    uint64_t t = sec;
    do {
        sec_str[sec_len++] = '0' + (t % 10);
        t /= 10;
    } while (t > 0 && sec_len < 7);

    int pos = 0;
    buf[pos++] = '[';
    int padding = 5 - sec_len;
    if (padding < 0) padding = 0;
    for (int i = 0; i < padding; i++) buf[pos++] = ' ';
    for (int i = sec_len - 1; i >= 0; i--) buf[pos++] = sec_str[i];

    buf[pos++] = '.';

    uint64_t div = 100000;
    while (div > 0) {
        buf[pos++] = '0' + (frac / div) % 10;
        div /= 10;
    }

    buf[pos++] = ']';
    buf[pos++] = ' ';
    buf[pos] = '\0';
}

void pit_init(uint32_t frequency) {
    if (frequency == 0 || frequency > PIT_BASE_FREQ) frequency = 1000;

    pit_divisor = PIT_BASE_FREQ / frequency;
    if (pit_divisor == 0 || pit_divisor > 65535) pit_divisor = 1193;

    // Channel 0, lobyte/hibyte, mode 2 (rate generator), 16-bit binary.
    // Mode 2, NOT mode 3 (0x36, square wave): in mode 3 the counter
    // decrements by 2 per cycle, so the countdown register is consumed at
    // 2x rate. pit_delay_ms and the poll-based uptime would then elapse in
    // half the nominal time (3 x 3s DNS timeouts finishing in 4.5s), and
    // the in-tick interpolation in pit_uptime_us runs 2x fast against the
    // pit_ticks base - every RTO/timeout judgement was skewed.
    outb(PIT_COMMAND, 0x34);

    // Send divisor (low byte then high byte)
    outb(PIT_CHANNEL0, pit_divisor & 0xFF);
    outb(PIT_CHANNEL0, (pit_divisor >> 8) & 0xFF);

    tsc_calibrate();

    // NOTE: IRQ0 is unmasked separately via pic_enable_irq(0) after the
    // PIC remap (pic_init masks all IRQ lines). pit_init itself may run
    // before interrupts_init() to start the timestamp clock as early as
    // possible - only the counter starts; IRQ0 delivery follows later.
}

/* Calibrate the TSC against the polled PIT countdown over a ~20ms window.
 * The PIT poll clock only serves here (and as the pre-calibration clock);
 * afterwards pit_uptime_us() is pure TSC - monotonic and IF-independent.
 * Bounded: a dead/slow PIT bails out and the poll clock stays in use. */
static void tsc_calibrate(void) {
    tsc_base = pit_rdtsc();

    uint64_t t0_us = pit_poll_elapsed_us();
    uint32_t guard = 1000000;                       /* hard bound */
    while (pit_poll_elapsed_us() - t0_us < 20000) {
        if (--guard == 0) return;                   /* PIT broken: fallback */
    }
    uint64_t elapsed_us = pit_poll_elapsed_us() - t0_us;
    uint64_t delta_tsc = pit_rdtsc() - tsc_base;

    if (elapsed_us < 5000) return;                  /* window too short */
    tsc_per_us = delta_tsc / elapsed_us;
    if (tsc_per_us == 0) return;                    /* absurd: fallback */
    tsc_ready = 1;
}

static uint16_t pit_read_counter(void) {
    /* The latch + two-byte read must be ATOMIC against other readers.
     * pit_uptime_us/pit_poll_elapsed_us/pit_delay_ms run from both
     * mainline and IRQ context; interleaved reads reassemble the low and
     * high bytes from two different instants, producing a count ABOVE
     * pit_divisor. In tick mode (pit_divisor - cur) is computed as
     * uint32 and wraps to ~4.29e9, launching uptime by hours in one
     * call - every RTO/timeout then fires immediately (observed as TCP
     * connect/send timeouts a few ms after start). */
    uint64_t flags = irq_save();
    disable_interrupts();
    outb(PIT_COMMAND, 0x00); // latch counter 0
    uint8_t lo = inb(PIT_CHANNEL0);
    uint8_t hi = inb(PIT_CHANNEL0);
    irq_restore(flags);
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

void pit_delay_ms(uint32_t ms) {
    if (ms == 0) return;

    uint32_t ticks_needed = (PIT_BASE_FREQ / 1000) * ms;
    uint32_t ticks_elapsed = 0;
    uint16_t last = pit_read_counter();

    while (ticks_elapsed < ticks_needed) {
        uint16_t cur = pit_read_counter();
        uint16_t delta;
        if (cur <= last) {
            delta = last - cur;
        } else {
            delta = last + (pit_divisor - cur);
        }
        ticks_elapsed += delta;
        last = cur;
    }
}