#ifndef TFTP_H
#define TFTP_H

#include "lib/types.h"
#include "net/netif.h"

/* Trivial FTP client (RFC 1350, octet mode only). Used by the shell
 * `tftp` command to pull binaries (Phase 1.4) and install them to /bin. */

#define TFTP_PORT 69

/* Downloads `filename` from `server_ip`. On success returns 0 and hands
 * the caller a kmalloc'd buffer in *out_buf (size *out_size); the caller
 * owns it. Returns -1 on any error (message via klog). */
int tftp_download(netif_t* iface, uint32_t server_ip, const char* filename,
                  uint8_t** out_buf, size_t* out_size);

#endif
