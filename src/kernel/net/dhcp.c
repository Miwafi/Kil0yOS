#include "net/dhcp.h"
#include "net/udp.h"
#include "net/ipv4.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "drivers/vga.h"
#define DHCP_BOOT_REQUEST  1
#define DHCP_BOOT_REPLY    2
#define DHCP_HTYPE_ETHER   1

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define OPT_SUBNET_MASK  1
#define OPT_ROUTER       3
#define OPT_MSG_TYPE     53
#define OPT_REQ_IP       50
#define OPT_SERVER_ID    54
#define OPT_END          255

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

typedef struct dhcp_header {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[312];
} __attribute__((packed)) dhcp_header_t;

/* host order fields stored as-is; wire order fixed at send time */
#define DHCP_MAGIC 0x63825363

static uint32_t dhcp_xid;

/* All multi-byte header fields are kept in host order and byte-swapped
 * on the way out (mirrors udp.c's approach). */
static int dhcp_send(netif_t* iface, uint8_t msg_type,
                     uint32_t req_ip, uint32_t server_id) {
    udp_socket_t* sock = udp_socket_create();
    if (!sock) return -1;
    udp_bind(sock, DHCP_CLIENT_PORT);

    uint8_t buf[sizeof(dhcp_header_t)];
    memset(buf, 0, sizeof(buf));

    dhcp_header_t* d = (dhcp_header_t*)buf;
    d->op    = DHCP_BOOT_REQUEST;
    d->htype = DHCP_HTYPE_ETHER;
    d->hlen  = NET_MAC_LEN;
    d->xid   = net_htonl(dhcp_xid);
    d->flags = net_htons(0x8000); /* broadcast */
    memcpy(d->chaddr, iface->mac, NET_MAC_LEN);
    d->magic = net_htonl(DHCP_MAGIC);

    uint8_t* o = d->options;
    *o++ = OPT_MSG_TYPE; *o++ = 1; *o++ = msg_type;
    if (req_ip != 0) {
        *o++ = OPT_REQ_IP; *o++ = 4;
        memcpy(o, &req_ip, 4); o += 4; /* host order, swapped below */
    }
    if (server_id != 0) {
        *o++ = OPT_SERVER_ID; *o++ = 4;
        memcpy(o, &server_id, 4); o += 4;
    }
    *o++ = OPT_END;

    /* swap the host-order 32-bit option values to wire order */
    if (req_ip != 0) {
        uint8_t* p = d->options + 3 + 2; /* after (50,len) */
        *(uint32_t*)(void*)p = net_htonl(req_ip);
    }
    if (server_id != 0) {
        uint8_t* p = d->options + 3 + 2 + 4 + 2; /* after (54,len) */
        *(uint32_t*)(void*)p = net_htonl(server_id);
    }

    uint16_t len = (uint16_t)(sizeof(dhcp_header_t) - sizeof(d->options) + (o - d->options));
    int rc = udp_sendto(sock, 0xFFFFFFFF, DHCP_SERVER_PORT, buf, len);
    udp_socket_close(sock);
    return rc;
}

/* Wait for a DHCP reply with matching xid. Returns payload length or -1.
 * On success fills out_msg / yiaddr / subnet / router / server_id. */
static int dhcp_wait(netif_t* iface, uint8_t expect_type, uint32_t timeout_ms,
                     uint32_t* yiaddr, uint32_t* subnet, uint32_t* router,
                     uint32_t* server_id) {
    udp_socket_t* sock = udp_socket_create();
    if (!sock) return -1;
    udp_bind(sock, DHCP_CLIENT_PORT);

    uint8_t buf[NET_MAX_PACKET];
    int found = -1;

    /* Timeout driven by pit_uptime_us() - the same validated clock as the
     * boot-log timestamps. (pit_delay_ms under-delays in this context.) */
    uint64_t deadline = pit_uptime_us() + (uint64_t)timeout_ms * 1000ULL;
    for (;;) {
        netif_poll();
        if (sock->rx_count > 0) {
            uint16_t len = sock->rx_count;
            memcpy(buf, sock->rx_buf, len);
            sock->rx_count = 0;

            if (len < sizeof(dhcp_header_t) - sizeof(((dhcp_header_t*)0)->options)) goto check_deadline;
            dhcp_header_t* d = (dhcp_header_t*)buf;
            if (d->op != DHCP_BOOT_REPLY) goto check_deadline;
            if (net_ntohl(d->xid) != dhcp_xid) goto check_deadline;

            uint32_t offer_yiaddr = net_ntohl(d->yiaddr);

            /* walk options */
            uint8_t msg = 0, over = 0;
            uint32_t opt_subnet = 0, opt_router = 0, opt_server = 0;
            const uint8_t* o = d->options;
            const uint8_t* end = buf + len;
            /* option overload (52) handling: only file/sname skipped for simplicity */
            while (o + 1 <= end && *o != OPT_END) {
                if (*o == 0) { o++; continue; }
                uint8_t code = *o++;
                if (o >= end) break;
                uint8_t olen = *o++;
                if (o + olen > end) break;
                if (code == OPT_MSG_TYPE && olen >= 1)     msg = o[0];
                if (code == OPT_SUBNET_MASK && olen == 4)  memcpy(&opt_subnet, o, 4);
                if (code == OPT_ROUTER && olen >= 4)       memcpy(&opt_router, o, 4);
                if (code == OPT_SERVER_ID && olen == 4)    memcpy(&opt_server, o, 4);
                if (code == 52 && olen >= 1)               over = o[0];
                o += olen;
            }
            (void)over;

            if (msg != expect_type) goto check_deadline;

            *yiaddr = offer_yiaddr;
            /* option values arrive in wire order */
            if (opt_subnet) { opt_subnet = net_ntohl(opt_subnet); memcpy(subnet, &opt_subnet, 4); }
            if (opt_router) { opt_router = net_ntohl(opt_router); memcpy(router, &opt_router, 4); }
            if (opt_server) { opt_server = net_ntohl(opt_server); memcpy(server_id, &opt_server, 4); }
            found = (int)len;
            break;
        }

check_deadline:
        if (pit_uptime_us() >= deadline) break;
    }

    udp_socket_close(sock);
    return found;
}

int dhcp_autoconfig(netif_t* iface) {
    /* xid derived from MAC for uniqueness across reboots */
    dhcp_xid = ((uint32_t)iface->mac[2] << 24) | ((uint32_t)iface->mac[3] << 16) |
               ((uint32_t)iface->mac[4] << 8)  | (uint32_t)iface->mac[5];

    /* ---- DISCOVER -> OFFER ---- */
    if (dhcp_send(iface, DHCP_DISCOVER, 0, 0) < 0) {
        klog("dhcp: send DISCOVER failed\n");
        return -1;
    }

    uint32_t yiaddr = 0, subnet = 0, router = 0, server_id = 0;
    if (dhcp_wait(iface, DHCP_OFFER, 4000, &yiaddr, &subnet, &router, &server_id) < 0) {
        klog("dhcp: no OFFER received\n");
        return -1;
    }
    if (yiaddr == 0) {
        klog("dhcp: OFFER without yiaddr\n");
        return -1;
    }

    /* ---- REQUEST -> ACK ---- */
    if (dhcp_send(iface, DHCP_REQUEST, yiaddr, server_id) < 0) return -1;

    uint32_t ack_yiaddr = 0, ack_subnet = 0, ack_router = 0, ack_server = 0;
    if (dhcp_wait(iface, DHCP_ACK, 4000, &ack_yiaddr, &ack_subnet, &ack_router, &ack_server) < 0) return -1;

    iface->ip      = ack_yiaddr ? ack_yiaddr : yiaddr;
    iface->netmask = ack_subnet ? ack_subnet : (subnet ? subnet : 0xFFFFFF00);
    iface->gateway = ack_router ? ack_router : (router ? router : 0);
    return 0;
}
