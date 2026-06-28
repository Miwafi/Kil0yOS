#ifndef ICMP_H
#define ICMP_H

#include "lib/types.h"
#include "net/netif.h"

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

typedef struct __attribute__((packed)) icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_header_t;

void icmp_receive(netif_t* iface, uint32_t src_ip, const uint8_t* data, uint16_t len);
int  icmp_ping(netif_t* iface, uint32_t target_ip, uint16_t seq, uint32_t timeout_ms);

#endif
