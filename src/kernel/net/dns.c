/* Phase 4.4: minimal kernel DNS resolver (A queries, UDP/53). */

#include "net/dns.h"
#include "net/udp.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/vga.h"

#define DNS_SERVER_PORT 53
#define DNS_QUERY       0x0100u   /* RD=1 */
#define DNS_TYPE_A      1
#define DNS_CLASS_IN    1
#define DNS_RETRIES     3
#define DNS_TIMEOUT_MS  3000

/* static fallback when DHCP gave no resolver: QEMU slirp DNS proxy */
#define DNS_FALLBACK_IP 0x0A000203u   /* 10.0.2.3 */

static uint16_t dns_next_id(void) {
    static uint16_t counter = 0;
    counter++;
    return (uint16_t)(pit_uptime_us() >> 4) ^ counter;
}

/* Encode "a.b.c" as DNS QNAME labels at out; returns bytes written. */
static int dns_build_qname(const char* name, uint8_t* out, size_t cap) {
    size_t off = 0;
    const char* p = name;
    while (*p) {
        const char* dot = strchr(p, '.');
        size_t llen = dot ? (size_t)(dot - p) : strlen(p);
        if (llen == 0 || llen > 63) return -1;       /* empty/double dot */
        if (off + 1 + llen + 1 > cap) return -1;
        out[off++] = (uint8_t)llen;
        memcpy(out + off, p, llen);
        off += llen;
        p += llen;
        if (*p == '.') p++;
        else if (*p) return -1;
    }
    if (off == 0) return -1;
    out[off++] = 0;                                   /* root label */
    return (int)off;
}

/* Skip a (possibly compressed) domain name; returns pointer past it. */
static const uint8_t* dns_skip_name(const uint8_t* p, const uint8_t* end) {
    while (p < end) {
        uint8_t l = *p;
        if (l == 0) return p + 1;
        if ((l & 0xC0) == 0xC0) return p + 2;         /* compression ptr */
        p += 1 + (size_t)l;
    }
    return end;
}

uint32_t dns_resolve(netif_t* iface, const char* name) {
    if (iface == NULL || name == NULL || !name[0]) return 0;

    uint32_t dns_ip = iface->dns ? iface->dns : DNS_FALLBACK_IP;

    uint8_t qname[256];
    int qname_len = dns_build_qname(name, qname, sizeof(qname));
    if (qname_len < 0) return 0;

    udp_socket_t* sock = udp_socket_create();
    if (sock == NULL) return 0;

    uint32_t result = 0;
    for (int attempt = 0; attempt < DNS_RETRIES && result == 0; attempt++) {
        uint16_t id = dns_next_id();

        uint8_t query[12 + 256 + 4];
        /* header */
        query[0] = (uint8_t)(id >> 8);  query[1] = (uint8_t)id;
        query[2] = (uint8_t)(DNS_QUERY >> 8); query[3] = (uint8_t)DNS_QUERY;
        query[4] = 0; query[5] = 1;               /* QDCOUNT = 1 */
        query[6] = 0; query[7] = 0;               /* ANCOUNT */
        query[8] = 0; query[9] = 0;               /* NSCOUNT */
        query[10] = 0; query[11] = 0;             /* ARCOUNT */
        memcpy(query + 12, qname, (size_t)qname_len);
        size_t qlen = 12 + (size_t)qname_len;
        query[qlen++] = 0; query[qlen++] = DNS_TYPE_A;
        query[qlen++] = 0; query[qlen++] = DNS_CLASS_IN;

        if (udp_sendto(sock, dns_ip, DNS_SERVER_PORT, query, (uint16_t)qlen) < 0)
            break;

        for (;;) {
            uint8_t resp[512];
            uint32_t src_ip = 0;
            uint16_t src_port = 0;
            int n = udp_recvfrom(sock, resp, sizeof(resp), &src_ip, &src_port,
                                 DNS_TIMEOUT_MS);
            if (n < 12 || src_ip != dns_ip) break;   /* timeout / wrong src */

            uint16_t rid   = (uint16_t)((resp[0] << 8) | resp[1]);
            uint16_t flags = (uint16_t)((resp[2] << 8) | resp[3]);
            uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
            if (rid != id) continue;                 /* stale reply */
            if (!(flags & 0x8000) || (flags & 0x000F) != 0) break; /* not reply/err */
            if (ancount == 0) break;

            /* skip the question section */
            const uint8_t* p = dns_skip_name(resp + 12, resp + n);
            p += 4;                                  /* QTYPE + QCLASS */
            if (p > resp + n) break;

            /* walk answers, return the first A record */
            for (uint16_t a = 0; a < ancount && result == 0; a++) {
                p = dns_skip_name(p, resp + n);
                if (p + 10 > resp + n) break;
                uint16_t type   = (uint16_t)((p[0] << 8) | p[1]);
                uint16_t rdlen  = (uint16_t)((p[8] << 8) | p[9]);
                p += 10;
                if (p + rdlen > resp + n) break;
                if (type == DNS_TYPE_A && rdlen == 4) {
                    result = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                             ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
                }
                p += rdlen;
            }
            break;                                   /* single query per attempt */
        }
    }

    udp_socket_close(sock);
    if (result == 0) {
        klog("[dns] resolve failed: ");
        klog(name);
        klog("\n");
    }
    return result;
}
