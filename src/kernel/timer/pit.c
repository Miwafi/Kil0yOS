#include "timer/pit.h"
#include "drivers/io.h"
#include "core/interrupts.h"

#define PIT_COMMAND   0x43
#define PIT_CHANNEL0  0x40
#define PIT_CHANNEL2  0x42
#define PIT_BASE_FREQ 1193180

static uint32_t pit_divisor = 1193;

volatile uint64_t pit_ticks = 0;

void pit_format_time(char* buf, size_t len) {
    if (len < 20) { buf[0] = '\0'; return; }

    uint64_t ticks = pit_ticks;
    uint64_t sec = ticks / 100;
    uint64_t usec = (ticks % 100) * 10000ULL;

    char sec_str[16];
    int sec_len = 0;
    uint64_t t = sec;
    do {
        sec_str[sec_len++] = '0' + (t % 10);
        t /= 10;
    } while (t > 0);

    int pos = 0;
    buf[pos++] = '[';
    int padding = 5 - sec_len;
    if (padding < 0) padding = 0;
    for (int i = 0; i < padding; i++) buf[pos++] = ' ';
    for (int i = sec_len - 1; i >= 0; i--) buf[pos++] = sec_str[i];

    buf[pos++] = '.';

    uint64_t div = 100000;
    while (div > 0) {
        buf[pos++] = '0' + (usec / div) % 10;
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