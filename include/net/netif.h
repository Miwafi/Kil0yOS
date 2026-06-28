#ifndef NETIF_H
#define NETIF_H

#include "lib/types.h"

#define NET_MAC_LEN     6
#define NET_MTU         1500
#define NET_MAX_PACKET  2048
#define NET_RX_QUEUE_SZ 64

static inline uint16_t net_htons(uint16_t v) {
    return (v >> 8) | ((v & 0xFF) << 8);
}
static inline uint32_t net_htonl(uint32_t v) {
    return ((v >> 24) & 0xFF) |
           ((v >> 8)  & 0xFF00) |
           ((v << 8)  & 0xFF0000) |
           ((v << 24) & 0xFF000000);
}
#define net_ntohs net_htons
#define net_ntohl net_htonl

typedef struct net_packet {
    uint8_t data[NET_MAX_PACKET];
    uint16_t len;
} net_packet_t;

typedef struct netif {
    uint8_t  mac[NET_MAC_LEN];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    int      flags;
    int      tx_pending;
    int      (*send)(const uint8_t* data, uint16_t len);
    void     (*poll)(void);
} netif_t;

extern netif_t g_netif;

int  netif_init(void);
const char* netif_probe(void);
void netif_poll(void);
void netif_receive(const uint8_t* data, uint16_t len);
int  netif_send(const uint8_t* data, uint16_t len);
int  netif_tx_ready(void);
void netif_get_mac(uint8_t* out_mac);
uint32_t netif_get_ip(void);

#endif
