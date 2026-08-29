#include "net/icmp.h"
#include "net/ipv4.h"
#include "timer/pit.h"
#include "lib/string.h"

static volatile int ping_reply = 0;
static volatile uint16_t ping_seq = 0;
static volatile uint32_t ping_peer = 0;
#define PING_ID 0x1234

void icmp_receive(netif_t* iface, uint32_t src_ip, const uint8_t* data, uint16_t len) {
    (void)iface;
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
        /* Belt and braces: len is already capped by netif_receive, but the
         * copy below writes into a fixed stack buffer - never trust it. */
        uint16_t payload_len = len - sizeof(icmp_header_t);
        if (payload_len > NET_MAX_PACKET - sizeof(icmp_header_t)) {
            payload_len = NET_MAX_PACKET - sizeof(icmp_header_t);
        }
        memcpy(reply_buf + sizeof(icmp_header_t),
               data + sizeof(icmp_header_t), payload_len);
        uint16_t cs_len = sizeof(icmp_header_t) + payload_len;
        reply->checksum = ip_checksum(reply, cs_len);
        ipv4_transmit(iface, src_ip, IP_PROTO_ICMP, reply_buf, cs_len);
    } else if (type == ICMP_TYPE_ECHO_REPLY) {
        uint16_t seq = net_ntohs(hdr.seq);
        uint16_t id  = net_ntohs(hdr.id);
        /* Match our own outstanding ping only: id, sequence AND peer. */
        if (seq == ping_seq && id == PING_ID && src_ip == ping_peer) {
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
    hdr->id = net_htons(PING_ID);
    hdr->seq = net_htons(seq);

    const char* payload = "Kil0yOS";
    uint16_t payload_len = 8;
    memcpy(buf + sizeof(icmp_header_t), payload, payload_len);
    uint16_t total_len = sizeof(icmp_header_t) + payload_len;
    hdr->checksum = ip_checksum(hdr, total_len);

    ping_reply = 0;
    ping_seq = seq;
    ping_peer = target_ip;

    uint64_t start = pit_uptime_us();
    uint64_t deadline = start + (uint64_t)timeout_ms * 1000ULL;

    if (ipv4_transmit(iface, target_ip, IP_PROTO_ICMP, buf, total_len) < 0) {
        return -1;
    }

    /* Wait for the reply, polling the NIC (works with and without a
     * working interrupt line). */
    while (pit_uptime_us() < deadline) {
        if (ping_reply) break;
        netif_poll();
        pit_delay_ms(10);
    }

    /* Real ping paces one request per second: pad a fast reply so the
     * next request goes out ~1s after this one was sent. */
    uint64_t min_period = 1000000ULL; /* 1s */
    while (pit_uptime_us() - start < min_period) {
        netif_poll();
        pit_delay_ms(10);
    }

    return ping_reply ? 0 : -1;
}
