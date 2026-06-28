#include "net/ipv4.h"
#include "net/arp.h"
#include "net/ethernet.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "lib/string.h"

uint16_t ip_checksum(const void* data, uint16_t len) {
    const uint8_t* buf = data;
    uint32_t sum = 0;
    for (uint16_t i = 0; i < len; i += 2) {
        uint16_t word = buf[i];
        if (i + 1 < len) word |= ((uint16_t)buf[i + 1] << 8);
        sum += word;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

void ipv4_receive(netif_t* iface, const uint8_t* src_mac, const uint8_t* data, uint16_t len) {
    if (len < sizeof(ipv4_header_t)) return;

    ipv4_header_t hdr;
    memcpy(&hdr, data, sizeof(hdr));

    uint8_t version = (hdr.ihl_version >> 4) & 0x0F;
    uint8_t ihl = hdr.ihl_version & 0x0F;
    if (version != 4) return;

    uint16_t total_len = (hdr.total_len >> 8) | ((hdr.total_len & 0xFF) << 8);
    uint16_t hdr_len = ihl * 4;
    if (total_len > len) total_len = len;
    if (total_len < hdr_len) return;

    uint32_t src_ip = net_ntohl(hdr.src);
    arp_cache_update(src_ip, src_mac);

    if (net_ntohl(hdr.dst) != iface->ip && hdr.dst != 0xFFFFFFFF) return;

    const uint8_t* payload = data + hdr_len;
    uint16_t payload_len = total_len - hdr_len;

    if (hdr.proto == IP_PROTO_ICMP) {
        icmp_receive(iface, src_ip, payload, payload_len);
    } else if (hdr.proto == IP_PROTO_UDP) {
        udp_receive(iface, src_ip, payload, payload_len);
    }
}

int ipv4_transmit(netif_t* iface, uint32_t dst_ip, uint8_t proto,
                  const uint8_t* payload, uint16_t payload_len) {
    uint8_t packet[NET_MAX_PACKET];
    uint16_t hdr_len = sizeof(ipv4_header_t);
    uint16_t total_len = hdr_len + payload_len;
    if (total_len > NET_MAX_PACKET) return -1;

    ipv4_header_t* hdr = (ipv4_header_t*)packet;
    hdr->ihl_version = (4 << 4) | 5;
    hdr->tos = 0;
    hdr->total_len = (total_len >> 8) | ((total_len & 0xFF) << 8);
    hdr->id = 0;
    hdr->flags_frag = 0x0040; /* Don't fragment */
    hdr->ttl = 64;
    hdr->proto = proto;
    hdr->checksum = 0;
    hdr->src = net_htonl(iface->ip);
    hdr->dst = net_htonl(dst_ip);
    hdr->checksum = ip_checksum(hdr, hdr_len);

    memcpy(packet + hdr_len, payload, payload_len);

    uint8_t dst_mac[6];
    if (arp_resolve(iface, dst_ip, dst_mac) < 0) {
        return -1;
    }

    return eth_transmit(iface, dst_mac, ETH_TYPE_IP, packet, total_len);
}
