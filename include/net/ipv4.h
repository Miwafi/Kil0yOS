#ifndef IPV4_H
#define IPV4_H

#include "lib/types.h"
#include "net/netif.h"

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct __attribute__((packed)) ipv4_header {
    uint8_t  ihl_version;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} ipv4_header_t;

void ipv4_receive(netif_t* iface, const uint8_t* src_mac, const uint8_t* data, uint16_t len);
int  ipv4_transmit(netif_t* iface, uint32_t dst_ip, uint8_t proto,
                   const uint8_t* payload, uint16_t payload_len);

uint16_t ip_checksum(const void* data, uint16_t len);

#endif
