#include "net/ethernet.h"
#include "net/arp.h"
#include "net/ipv4.h"
#include "lib/string.h"

void eth_receive(netif_t* iface, const uint8_t* data, uint16_t len) {
    if (len < ETH_HDR_LEN) return;

    const eth_header_t* eth = (const eth_header_t*)data;
    uint16_t type = (eth->type >> 8) | ((eth->type & 0xFF) << 8);
    const uint8_t* payload = data + ETH_HDR_LEN;
    uint16_t payload_len = len - ETH_HDR_LEN;

    if (type == ETH_TYPE_ARP) {
        arp_receive(iface, payload, payload_len);
    } else if (type == ETH_TYPE_IP) {
        ipv4_receive(iface, eth->src, payload, payload_len);
    }
}

int eth_transmit(netif_t* iface, const uint8_t* dst_mac, uint16_t type,
                 const uint8_t* payload, uint16_t payload_len) {
    if (payload_len > NET_MTU) return -1;

    uint8_t packet[NET_MAX_PACKET];
    eth_header_t* eth = (eth_header_t*)packet;

    for (int i = 0; i < 6; i++) {
        eth->dst[i] = dst_mac[i];
        eth->src[i] = iface->mac[i];
    }
    eth->type = (type >> 8) | ((type & 0xFF) << 8);

    memcpy(packet + ETH_HDR_LEN, payload, payload_len);
    return netif_send(packet, ETH_HDR_LEN + payload_len);
}
