#ifndef NET_HTTP_H
#define NET_HTTP_H

#include "lib/types.h"
#include "net/netif.h"

/* Minimal HTTP/1.1 GET client over the Phase 3.3 TCP stack (Phase 4.4).
 * Synchronous (spin + tcp_poll), used by the kilget repo client. */

/* Download http://host[:port]/path into a heap buffer (caller kfrees).
 * host: dotted IP or a name resolved via /etc/hosts. port 0 = 80.
 * On success returns 0 and sets *out and *out_len to the response body. */
int http_download(netif_t* iface, const char* host, uint16_t port,
                  const char* path, uint8_t** out, size_t* out_len);

#endif
