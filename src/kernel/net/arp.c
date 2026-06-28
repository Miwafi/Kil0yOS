#include "net/arp.h"
#include "net/ethernet.h"
#include "net/netif.h"
#include "timer/pit.h"
#include "lib/string.h"

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static int arp_cache_count = 0;

void arp_init(void) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = 0;
    }
    arp_cache_count = 0;
}

static arp_entry_t* arp_find(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            return &arp_cache[i];
        }
    }
    return NULL;
}

void arp_cache_update(uint32_t ip, const uint8_t* mac) {
    arp_entry_t* entry = arp_find(ip);
    if (entry) {
        for (int i = 0; i < 6; i++) entry->mac[i] = mac[i];
        entry->timestamp = 0; /* simplified */
        return;
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            arp_cache[i].valid = 1;
            arp_cache[i].timestamp = 0;
            return;
        }
    }
    /* evict first entry if full */
    arp_cache[0].ip = ip;
    for (int j = 0; j < 6; j++) arp_cache[0].mac[j] = mac[j];
    arp_cache[0].timestamp = 0;
}

void arp_receive(netif_t* iface, const uint8_t* data, uint16_t len) {
    if (len < sizeof(arp_packet_t)) return;

    const arp_packet_t* arp = (const arp_packet_t*)data;
    uint16_t htype = (arp->htype >> 8) | ((arp->htype & 0xFF) << 8);
    uint16_t ptype = (arp->ptype >> 8) | ((arp->ptype & 0xFF) << 8);
    uint16_t oper  = (arp->oper  >> 8) | ((arp->oper  & 0xFF) << 8);

    if (htype != ARP_HTYPE_ETH || ptype != ARP_PTYPE_IP) return;

    arp_cache_update(net_ntohl(arp->spa), arp->sha);

    if (oper == ARP_OP_REQUEST && net_ntohl(arp->tpa) == iface->ip) {
        arp_packet_t reply;
        reply.htype = arp->htype;
        reply.ptype = arp->ptype;
        reply.hlen  = 6;
        reply.plen  = 4;
        reply.oper  = (ARP_OP_REPLY >> 8) | ((ARP_OP_REPLY & 0xFF) << 8);
        for (int i = 0; i < 6; i++) reply.sha[i] = iface->mac[i];
        reply.spa = net_htonl(iface->ip);
    for (int i = 0; i < 6; i++) reply.tha[i] = arp->sha[i];
    reply.tpa = arp->spa;
        eth_transmit(iface, arp->sha, ETH_TYPE_ARP, (uint8_t*)&reply, sizeof(reply));
    }
}

void arp_send_request(netif_t* iface, uint32_t target_ip) {
    arp_packet_t req;
    req.htype = (ARP_HTYPE_ETH >> 8) | ((ARP_HTYPE_ETH & 0xFF) << 8);
    req.ptype = (ARP_PTYPE_IP >> 8) | ((ARP_PTYPE_IP & 0xFF) << 8);
    req.hlen  = 6;
    req.plen  = 4;
    req.oper  = (ARP_OP_REQUEST >> 8) | ((ARP_OP_REQUEST & 0xFF) << 8);
    for (int i = 0; i < 6; i++) req.sha[i] = iface->mac[i];
    req.spa = net_htonl(iface->ip);
    for (int i = 0; i < 6; i++) req.tha[i] = 0x00;
    req.tpa = net_htonl(target_ip);

    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    eth_transmit(iface, broadcast, ETH_TYPE_ARP, (uint8_t*)&req, sizeof(req));
}

int arp_resolve(netif_t* iface, uint32_t ip, uint8_t* out_mac) {
    if (ip == 0xFFFFFFFF) {
        for (int i = 0; i < 6; i++) out_mac[i] = 0xFF;
        return 0;
    }

    uint32_t net = iface->ip & iface->netmask;
    uint32_t target_net = ip & iface->netmask;
    if (net != target_net && iface->gateway != 0) {
        ip = iface->gateway;
    }

    arp_entry_t* entry = arp_find(ip);
    if (entry) {
        for (int i = 0; i < 6; i++) out_mac[i] = entry->mac[i];
        return 0;
    }

    arp_send_request(iface, ip);

    for (int retry = 0; retry < 50; retry++) {
        pit_delay_ms(2);
        netif_poll();
        entry = arp_find(ip);
        if (entry) {
            for (int i = 0; i < 6; i++) out_mac[i] = entry->mac[i];
            return 0;
        }
    }
    return -1;
}

const arp_entry_t* arp_get_cache(int* count) {
    *count = 0;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid) (*count)++;
    }
    return arp_cache;
}
