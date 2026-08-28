#ifndef DHCP_H
#define DHCP_H

#include "net/netif.h"

/* Auto-configure the interface via DHCP (DISCOVER -> OFFER -> REQUEST -> ACK).
 * On success fills iface->ip / netmask / gateway and returns 0.
 * Returns -1 on failure (no reply / no offer); caller applies a fallback. */
int dhcp_autoconfig(netif_t* iface);

#endif
