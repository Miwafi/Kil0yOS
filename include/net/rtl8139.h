#ifndef RTL8139_H
#define RTL8139_H

#include "lib/types.h"

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

int rtl8139_init(void);
int rtl8139_send(const uint8_t* data, uint16_t len);
void rtl8139_get_mac(uint8_t* out_mac);

#endif
