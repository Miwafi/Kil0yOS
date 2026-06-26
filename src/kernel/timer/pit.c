#include "timer/pit.h"
#include "drivers/io.h"
#include "core/interrupts.h"

#define PIT_COMMAND   0x43
#define PIT_CHANNEL0  0x40
#define PIT_BASE_FREQ 1193180

void pit_init(uint32_t frequency) {
    if (frequency == 0 || frequency > PIT_BASE_FREQ) frequency = 1000;

    uint32_t divisor = PIT_BASE_FREQ / frequency;
    if (divisor == 0 || divisor > 65535) divisor = 1193;

    // Channel 0, lobyte/hibyte, mode 2 (rate generator), 16-bit binary
    outb(PIT_COMMAND, 0x36);

    // Send divisor (low byte then high byte)
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    // Enable IRQ 0 in PIC
    pic_enable_irq(0);
}