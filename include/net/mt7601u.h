#ifndef MT7601U_H
#define MT7601U_H

#include "usb/usb.h"

/* MediaTek MT7601U USB 802.11n Wi-Fi dongle driver (phase 1: bring-up).
 *
 * Phase 1 covers everything up to a running MCU: vendor register access,
 * bulk endpoint assignment, firmware upload (embedded mt7601u.bin) and
 * eFuse MAC readout.  Phase 2 (802.11 station + WPA2 + netif glue) builds
 * on these primitives.  Sequences are ported 1:1 from Linux
 * drivers/net/wireless/mediatek/mt7601u (GPL-2.0). */

/* USB IDs shared with the Linux driver's device table */
int mt7601u_matches(uint16_t vendor_id, uint16_t product_id);

/* bring-up: wait ASIC, verify revision, upload firmware, read MAC.
 * Blocking (several 100 ms); runs in the enumeration (IF=0) context. */
int  mt7601u_attach(usb_device_t* dev);
void mt7601u_detach(usb_device_t* dev);

/* status for the shell `usb` command */
void mt7601u_dump(usb_device_t* dev);

/* primitives exposed for phase 2 (MLME/data path) */
int  mt7601u_rr(usb_device_t* dev, uint16_t offset, uint32_t* value);
int  mt7601u_wr(usb_device_t* dev, uint16_t offset, uint32_t value);
int  mt7601u_bulk_out(usb_device_t* dev, const uint8_t* data, uint16_t len);
int  mt7601u_bulk_in(usb_device_t* dev, uint8_t* data, uint16_t len,
                     uint16_t* recv);

/* firmware blob (linked from assets/firmware/mt7601u.bin, incbin) */
extern const uint8_t mt7601u_fw_start[];
extern const uint8_t mt7601u_fw_end[];

#endif
