#ifndef ARP_H
#define ARP_H

#include "lib/types.h"
#include "net/netif.h"

#define ARP_HTYPE_ETH 1
#define ARP_PTYPE_IP  0x0800
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

#define ARP_CACHE_SIZE 16
#define ARP_TIMEOUT_MS 30000

typedef struct __attribute__((packed)) arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint32_t spa;
    uint8_t  tha[6];
    uint32_t tpa;
} arp_packet_t;

typedef struct arp_entry {
    uint32_t ip;
    uint8_t  mac[6];
    uint32_t timestamp;
    int      valid;
} arp_entry_t;

void arp_init(void);
void arp_receive(netif_t* iface, const uint8_t* data, uint16_t len);
int  arp_resolve(netif_t* iface, uint32_t ip, uint8_t* out_mac);
void arp_send_request(netif_t* iface, uint32_t target_ip);
void arp_cache_update(uint32_t ip, const uint8_t* mac);
const arp_entry_t* arp_get_cache(int* count);

#endif
