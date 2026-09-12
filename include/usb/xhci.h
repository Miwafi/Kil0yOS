#ifndef XHCI_H
#define XHCI_H

#include "usb/usb.h"
#include "drivers/pci.h"

/* xHCI (USB 3.0) host controller driver - phase 1: keyboard.
 *
 * xHCI natively handles full-, high- and super-speed devices on its root
 * ports, which is what modern real machines need (their boards have no
 * UHCI companion, and the EHCI spec forbids driving a full-speed device
 * from a root port - see ehci.c).  Design mirrors the rest of the USB
 * stack: poll-based (interrupter IE left clear, events drained from
 * usb_tick and from blocking waits), one control transfer in flight.
 *
 * Structures: one DCBAA page; per-slot device-context page and input
 * context page; one command ring, one event ring (+ERST), and per-
 * endpoint transfer rings, all from PMM pages (identity map, < 4GB).
 */

int  xhci_init(pci_device_t* pci);              /* 0 = ok */
int  xhci_ready(void);
int  xhci_port_count(void);
int  xhci_port_connected(int port);             /* port = 1..n */
/* reset; returns xHCI speed code (1=FS 2=LS 3=HS 4=SS 5=SSP), 0 = none */
int  xhci_port_reset(int port);
void xhci_clear_port_change(int port);
void xhci_dump_controller(void);

/* enumerate a freshly reset port: EnableSlot + AddressDevice(BAA).
 * On success dev->address = slot id and ep0 is live. */
int  xhci_enumerate(usb_device_t* dev, int port, int speed);

/* control transfer over ep0 (dispatched by usb_control_xfer) */
int  xhci_control_xfer(usb_device_t* dev,
                       uint8_t bmReq, uint8_t bReq,
                       uint16_t wValue, uint16_t wIndex,
                       int dir_in,
                       uint8_t* data, uint16_t len,
                       uint16_t* recv_len);

/* configure an interrupt IN endpoint and start report delivery
 * (reports arrive through dev->on_report from the event ring) */
int  xhci_start_interrupt_in(usb_device_t* dev, uint8_t ep_addr,
                             uint16_t mps, uint8_t interval);
void xhci_stop_interrupt_in(usb_device_t* dev);

/* drain the event ring (usb_tick + inline waits) */
void xhci_poll(void);

#endif
