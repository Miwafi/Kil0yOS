#include "timer/pit.h"
#include "drivers/io.h"
#include "core/interrupts.h"

#define PIT_COMMAND   0x43
#define PIT_CHANNEL0  0x40
#define PIT_CHANNEL2  0x42
#define PIT_BASE_FREQ 1193180

static uint32_t pit_divisor = 1193;

void pit_init(uint32_t frequency) {
    if (frequency == 0 || frequency > PIT_BASE_FREQ) frequency = 1000;

    pit_divisor = PIT_BASE_FREQ / frequency;
    if (pit_divisor == 0 || pit_divisor > 65535) pit_divisor = 1193;

    // Channel 0, lobyte/hibyte, mode 2 (rate generator), 16-bit binary
    outb(PIT_COMMAND, 0x36);

    // Send divisor (low byte then high byte)
    outb(PIT_CHANNEL0, pit_divisor & 0xFF);
    outb(PIT_CHANNEL0, (pit_divisor >> 8) & 0xFF);

    // Enable IRQ 0 in PIC
    pic_enable_irq(0);
}

static uint16_t pit_read_counter(void) {
    outb(PIT_COMMAND, 0x00); // latch counter 0
    uint8_t lo = inb(PIT_CHANNEL0);
    uint8_t hi = inb(PIT_CHANNEL0);
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