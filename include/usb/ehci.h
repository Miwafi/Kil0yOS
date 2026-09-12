#ifndef EHCI_H
#define EHCI_H

#include "usb/usb.h"
#include "drivers/pci.h"

/* EHCI (USB 2.0) host controller - high-speed bulk/control only.
 *
 * The driver is deliberately minimal (poll-based, one outstanding
 * transfer at a time, no periodic schedule, no interrupt endpoints):
 * it exists to drive high-speed Wi-Fi dongles (MT7601U) whose bulk
 * pipes cannot be served by the UHCI companion.  Root-hub port events
 * are polled at ~1 s cadence by usb_tick, exactly like UHCI. */

int  ehci_init(pci_device_t* pci);              /* 0 = ok */
int  ehci_ready(void);
int  ehci_port_count(void);
int  ehci_port_connected(int port);             /* port = 1..n */
int  ehci_port_reset(int port);                 /* 0 = ok, high-speed device */
void ehci_clear_port_change(int port);
void ehci_dump_controller(void);

/* Blocking transfers (same semantics as the uhci_control_xfer API) */
int ehci_control_xfer(usb_device_t* dev,
                      uint8_t bmReq, uint8_t bReq,
                      uint16_t wValue, uint16_t wIndex,
                      int dir_in,
                      uint8_t* data, uint16_t len,
                      uint16_t* recv_len);
int ehci_bulk_xfer(usb_device_t* dev, uint8_t ep_num, int dir_in,
                   uint8_t* data, uint16_t len, uint16_t* recv_len);

#endif
