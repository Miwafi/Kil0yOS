#ifndef NET_DNS_H
#define NET_DNS_H

#include "lib/types.h"
#include "net/netif.h"

/* Minimal kernel-side DNS client (Phase 4.4): recursive A-record query
 * over UDP port 53, used by the HTTP client to resolve hostnames that
 * are neither a literal IP nor listed in /etc/hosts. */

/* Resolve name to the first A-record IPv4 address (host order).
 * Returns 0 on failure. Uses iface->dns as the resolver, falling back
 * to the QEMU slirp DNS proxy (10.0.2.3) when DHCP supplied none. */
uint32_t dns_resolve(netif_t* iface, const char* name);

#endif
