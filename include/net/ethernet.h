#ifndef ETHERNET_H
#define ETHERNET_H

#include "lib/types.h"
#include "net/netif.h"

#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IP   0x0800

#define ETH_HDR_LEN   14

typedef struct __attribute__((packed)) eth_header {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} eth_header_t;

void eth_receive(netif_t* iface, const uint8_t* data, uint16_t len);
int  eth_transmit(netif_t* iface, const uint8_t* dst_mac, uint16_t type,
                  const uint8_t* payload, uint16_t payload_len);

#endif
