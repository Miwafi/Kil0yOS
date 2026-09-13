#ifndef RTC_H
#define RTC_H

#include "lib/types.h"

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t century;
} rtc_time_t;

void rtc_init(void);
int rtc_read(rtc_time_t* time);

#endif
