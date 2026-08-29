#ifndef E1000_H
#define E1000_H

#include "lib/types.h"

#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID 0x100E       /* 82540EM (QEMU default) */
#define E1000_DEVICE_ID_82545 0x100F /* 82545EM (VMware "E1000") */
#define E1000_DEVICE_ID_82574 0x10D3 /* 82574L  (VMware "E1000E") */
#define E1000_DEVICE_ID_82574LA 0x10F6

int  e1000_init(void);
int  e1000_send(const uint8_t* data, uint16_t len);

#endif
