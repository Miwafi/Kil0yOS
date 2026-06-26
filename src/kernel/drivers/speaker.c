#include "drivers/speaker.h"
#include "drivers/io.h"
#include "timer/pit.h"

#define PIT_COMMAND   0x43
#define PIT_CHANNEL2  0x42
#define PIT_BASE_FREQ 1193180
#define SPEAKER_PORT  0x61

void speaker_init(void) {
    speaker_stop();
}

void speaker_play(uint32_t freq_hz) {
    if (freq_hz == 0) {
        speaker_stop();
        return;
    }

    uint32_t divisor = PIT_BASE_FREQ / freq_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor == 0) divisor = 1;

    // Channel 2, lobyte/hibyte, mode 3 (square wave), binary
    outb(PIT_COMMAND, 0xB6);

    outb(PIT_CHANNEL2, divisor & 0xFF);
    outb(PIT_CHANNEL2, (divisor >> 8) & 0xFF);

    uint8_t tmp = inb(SPEAKER_PORT);
    if ((tmp & 0x03) != 0x03) {
        outb(SPEAKER_PORT, tmp | 0x03);
    }
}

void speaker_stop(void) {
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & 0xFC);
}

void speaker_beep(uint32_t freq_hz, uint32_t duration_ms) {
    speaker_play(freq_hz);
    pit_delay_ms(duration_ms);
    speaker_stop();
}
