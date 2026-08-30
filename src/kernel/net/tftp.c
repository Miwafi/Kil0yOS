/* Phase 1.4: minimal TFTP client (read-only, octet mode).
 *
 * Packet formats (RFC 1350):
 *   RRQ   : 01 filename 0 "octet" 0
 *   DATA  : 03 block#  data[0..512]
 *   ACK   : 04 block#
 *   ERROR : 05 code  msg 0
 *
 * The server answers the initial RRQ from an ephemeral port ("transfer
 * ID"); all subsequent traffic must use it. We lock onto the source port
 * of the first packet we accept and send every ACK there.
 */
#include "net/tftp.h"
#include "net/udp.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/vga.h"

#define TFTP_RRQ   1
#define TFTP_DATA  3
#define TFTP_ACK   4
#define TFTP_ERROR 5

#define TFTP_DATA_SZ 512
#define TFTP_ATTEMPTS 5
#define TFTP_TIMEOUT_MS 1500

static int tftp_send_rrq(udp_socket_t* sock, const char* filename) {
    uint8_t pkt[4 + 128];
    size_t nlen = strlen(filename);
    if (nlen > 120) return -1;

    size_t i = 0;
    pkt[i++] = 0; pkt[i++] = TFTP_RRQ;
    memcpy(pkt + i, filename, nlen); i += nlen;
    pkt[i++] = 0;
    memcpy(pkt + i, "octet", 5); i += 5;
    pkt[i++] = 0;
    return udp_sendto(sock, sock->remote_ip, TFTP_PORT, pkt, (uint16_t)i);
}

static int tftp_send_ack(udp_socket_t* sock, uint16_t block) {
    uint8_t pkt[4];
    pkt[0] = 0; pkt[1] = TFTP_ACK;
    pkt[2] = (uint8_t)(block >> 8);
    pkt[3] = (uint8_t)block;
    return udp_sendto(sock, sock->remote_ip, sock->remote_port, pkt, 4);
}

int tftp_download(netif_t* iface, uint32_t server_ip, const char* filename,
                  uint8_t** out_buf, size_t* out_size) {
    (void)iface;
    *out_buf = NULL;
    *out_size = 0;

    udp_socket_t* sock = udp_socket_create();
    if (!sock) return -1;
    sock->remote_ip = server_ip;

    uint8_t* buf = NULL;
    size_t size = 0, cap = 0;
    uint16_t expected = 1;      /* next DATA block we want */
    int tids_seen = 0;          /* server transfer-ID locked? */
    uint16_t tid_port = 0;

    int rc = -1;
    for (int attempt = 0; attempt < TFTP_ATTEMPTS && rc != 0; attempt++) {
        klog("tftp: attempt ");
        char ab[8];
        itoa(attempt + 1, ab, 10, sizeof(ab));
        klog(ab);
        klog("\n");

        if (tftp_send_rrq(sock, filename) < 0) {
            /* Transient failures (e.g. ARP resolution timing out) should
             * not abort the whole transfer - retry the RRQ. */
            klog("tftp: RRQ send failed, retrying\n");
            continue;
        }
        klog("tftp: RRQ sent\n");

        /* Receive loop for the whole transfer; on timeout, re-send RRQ
         * (before the first DATA) or the last ACK (mid-transfer). */
        int timeouts = 0;
        for (;;) {
            uint8_t rx[TFTP_DATA_SZ + 8];
            uint32_t src_ip = 0;
            uint16_t src_port = 0;
            int n = udp_recvfrom(sock, rx, sizeof(rx), &src_ip, &src_port,
                                 TFTP_TIMEOUT_MS);
            if (n < 0) {
                /* timeout: retry the request / last ACK. The re-ACK path
                 * must be bounded - an unbounded loop here used to hang
                 * the shell forever when the server stopped sending. */
                if (size == 0 || expected == 1) {
                    break;  /* outer: re-RRQ */
                }
                if (++timeouts > TFTP_ATTEMPTS) {
                    klog("tftp: peer stalled, aborting\n");
                    goto out;
                }
                if (tftp_send_ack(sock, (uint16_t)(expected - 1)) < 0) {
                    goto out;
                }
                continue;
            }
            timeouts = 0;
            if (src_ip != server_ip) continue;
            if (n < 4) continue;
            uint16_t opcode = ((uint16_t)rx[0] << 8) | rx[1];
            uint16_t block  = ((uint16_t)rx[2] << 8) | rx[3];

            if (opcode == TFTP_ERROR) {
                klog("tftp: server error ");
                char b[12];
                itoa(block, b, 10, 12);
                klog(b);
                klog("\n");
                goto out;
            }
            if (opcode != TFTP_DATA) continue;
            if (!tids_seen) {
                tid_port = src_port;
                tids_seen = 1;
            } else if (src_port != tid_port) {
                continue;   /* stale duplicate from the well-known port */
            }

            int dlen = n - 4;
            if (block < expected) {
                /* duplicate of an already-acked block: re-ACK it */
                klog("tftp: dup block\n");
                tftp_send_ack(sock, block);
                continue;
            }
            if (block != expected) {
                char bb[80];
                char* p = bb;
                const char* s = "tftp: future blk ";
                while (*s) *p++ = *s++;
                itoa((int)block, p, 10, 12);
                while (*p) p++;
                s = " op=";
                while (*s) *p++ = *s++;
                itoa((int)opcode, p, 10, 12);
                while (*p) p++;
                s = " port=";
                while (*s) *p++ = *s++;
                itoa((int)src_port, p, 10, 12);
                while (*p) p++;
                s = " n=";
                while (*s) *p++ = *s++;
                itoa(n, p, 10, 12);
                while (*p) p++;
                *p++ = '\n'; *p = 0;
                klog(bb);
                continue;   /* future block: ignore */
            }

            if ((block & 0x0F) == 0) {
                char bb[12];
                klog("tftp: blk ");
                itoa((int)block, bb, 10, sizeof(bb));
                klog(bb);
                klog("\n");
            }

            if (size + (size_t)dlen > cap) {
                size_t ncap = cap ? cap * 2 : 8192;
                while (ncap < size + (size_t)dlen) ncap *= 2;
                uint8_t* nb = kmalloc(ncap);
                if (!nb) {
                    klog("tftp: out of memory\n");
                    goto out;
                }
                if (buf) {
                    memcpy(nb, buf, size);
                    kfree(buf);
                }
                buf = nb;
                cap = ncap;
            }
            memcpy(buf + size, rx + 4, (size_t)dlen);
            size += (size_t)dlen;

            if ((size & 0xFFFF) < (uint32_t)dlen) {   /* every 64 KiB */
                char nb[16];
                klog("tftp: ");
                itoa((int)(size >> 10), nb, 10, sizeof(nb));
                klog(nb);
                klog(" KiB\n");
            }

            if (tftp_send_ack(sock, block) < 0) goto out;
            expected++;

            if (dlen < TFTP_DATA_SZ) {
                rc = 0;   /* last block */
                break;
            }
        }
    }

    if (rc == 0) {
        *out_buf = buf;
        *out_size = size;
        buf = NULL;   /* ownership transferred */
    } else {
        klog("tftp: transfer failed\n");
    }

out:
    if (buf) kfree(buf);
    udp_socket_close(sock);
    return rc;
}
