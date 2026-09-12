#include "drivers/rtc.h"
#include "drivers/io.h"

#define CMOS_INDEX  0x70
#define CMOS_DATA   0x71

#define RTC_SEC     0x00
#define RTC_MIN     0x02
#define RTC_HOUR    0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32
#define RTC_STAT_A  0x0A
#define RTC_STAT_B  0x0B

#define RTC_B_24H   0x02
#define RTC_B_BIN   0x04

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

void rtc_init(void) {
}

int rtc_read(rtc_time_t* time) {
    if (!time) return -1;

    /* UIP-safe read.  The old `while (UIP);` was UNBOUNDED: the desktop
     * clock loop calls rtc_read ~100x/s, so it hits the per-second ~2ms
     * update window within seconds of entering the desktop - and with the
     * parked VMware firmware the observed UIP never cleared, silently
     * freezing the desktop (no #PF, interrupts still on, nothing on the
     * serial log).  Now: bounded UIP wait + full-burst retry when an
     * update begins mid-read (torn values) + -1 degrade instead of hang. */
    uint8_t second, minute, hour, day, month, year, century;
    int ok = 0;
    for (int attempt = 0; attempt < 5 && !ok; attempt++) {
        uint32_t n = 500000;   /* real silicon clears UIP in <= 2.244ms */
        while (cmos_read(RTC_STAT_A) & 0x80) {
            if (--n == 0) return -1;   /* UIP stuck: degrade, never hang */
            __asm__ volatile("pause");
        }

        second  = cmos_read(RTC_SEC);
        minute  = cmos_read(RTC_MIN);
        hour    = cmos_read(RTC_HOUR);
        day     = cmos_read(RTC_DAY);
        month   = cmos_read(RTC_MONTH);
        year    = cmos_read(RTC_YEAR);
        century = cmos_read(RTC_CENTURY);

        /* update began during the burst?  UIP set again -> values may be
         * torn (e.g. 59 -> 00 mid-read); retry the whole burst. */
        if ((cmos_read(RTC_STAT_A) & 0x80) == 0) ok = 1;
    }
    if (!ok) return -1;

    uint8_t stat_b = cmos_read(RTC_STAT_B);

    /* Convert BCD to binary if needed */
    if (!(stat_b & RTC_B_BIN)) {
        second  = bcd_to_bin(second);
        minute  = bcd_to_bin(minute);
        hour    = bcd_to_bin(hour & 0x7F);
        day     = bcd_to_bin(day);
        month   = bcd_to_bin(month);
        year    = bcd_to_bin(year);
        if (century != 0 && century != 0xFF) {
            century = bcd_to_bin(century);
        }
    }

    /* Handle 12-hour format */
    if (!(stat_b & RTC_B_24H)) {
        if (hour & 0x80) {
            hour = ((hour & 0x7F) % 12) + 12;
        } else {
            hour = (hour & 0x7F) % 12;
        }
    }

    time->second  = second;
    time->minute  = minute;
    time->hour    = hour;
    time->day     = day;
    time->month   = month;

    if (century == 0 || century == 0xFF) {
        time->year = 2000 + year;
    } else {
        time->year = century * 100 + year;
    }

    time->century = (time->year / 100);

    return 0;
}
