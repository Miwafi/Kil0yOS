#include "net/icmp.h"
#include "net/ipv4.h"
#include "timer/pit.h"
#include "lib/string.h"

static volatile int ping_reply = 0;
static volatile uint16_t ping_seq = 0;

void icmp_receive(netif_t* iface, uint32_t src_ip, const uint8_t* data, uint16_t len) {
    if (len < sizeof(icmp_header_t)) return;

    icmp_header_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    uint8_t type = hdr.type;

    if (type == ICMP_TYPE_ECHO_REQUEST) {
        uint8_t reply_buf[NET_MAX_PACKET];
        icmp_header_t* reply = (icmp_header_t*)reply_buf;
        reply->type = ICMP_TYPE_ECHO_REPLY;
        reply->code = 0;
        reply->checksum = 0;
        reply->id = hdr.id;
        reply->seq = hdr.seq;
        uint16_t payload_len = len - sizeof(icmp_header_t);
        memcpy(reply_buf + sizeof(icmp_header_t),
               data + sizeof(icmp_header_t), payload_len);
        uint16_t cs_len = sizeof(icmp_header_t) + payload_len;
        reply->checksum = ip_checksum(reply, cs_len);
        ipv4_transmit(iface, src_ip, IP_PROTO_ICMP, reply_buf, cs_len);
    } else if (type == ICMP_TYPE_ECHO_REPLY) {
        uint16_t seq = net_ntohs(hdr.seq);
        if (seq == ping_seq) {
            ping_reply = 1;
        }
    }
}

int icmp_ping(netif_t* iface, uint32_t target_ip, uint16_t seq, uint32_t timeout_ms) {
    uint8_t buf[128];
    icmp_header_t* hdr = (icmp_header_t*)buf;
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = net_htons(0x1234);
    hdr->seq = net_htons(seq);

    const char* payload = "Kil0yOS";
    uint16_t payload_len = 8;
    memcpy(buf + sizeof(icmp_header_t), payload, payload_len);
    uint16_t total_len = sizeof(icmp_header_t) + payload_len;
    hdr->checksum = ip_checksum(hdr, total_len);

    ping_reply = 0;
    ping_seq = seq;

    if (ipv4_transmit(iface, target_ip, IP_PROTO_ICMP, buf, total_len) < 0) {
        return -1;
    }

    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (ping_reply) return 0;
        netif_poll();
        pit_delay_ms(10);
        waited += 10;
    }
    return -1;
}
