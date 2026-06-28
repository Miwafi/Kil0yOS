#include "net/udp.h"
#include "net/ipv4.h"
#include "timer/pit.h"
#include "lib/string.h"

static udp_socket_t sockets[UDP_MAX_SOCKETS];

void udp_init(void) {
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        sockets[i].used = 0;
        sockets[i].bound = 0;
        sockets[i].rx_count = 0;
    }
}

void udp_receive(netif_t* iface, uint32_t src_ip, const uint8_t* data, uint16_t len) {
    (void)iface;
    if (len < sizeof(udp_header_t)) return;

    udp_header_t hdr;
    memcpy(&hdr, data, sizeof(hdr));
    uint16_t dst_port = (hdr.dst_port >> 8) | ((hdr.dst_port & 0xFF) << 8);
    uint16_t src_port = (hdr.src_port >> 8) | ((hdr.src_port & 0xFF) << 8);
    uint16_t udp_len  = (hdr.len >> 8) | ((hdr.len & 0xFF) << 8);
    if (udp_len > len) udp_len = len;
    uint16_t payload_len = udp_len - sizeof(udp_header_t);
    const uint8_t* payload = data + sizeof(udp_header_t);

    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].bound && sockets[i].local_port == dst_port) {
            if (sockets[i].rx_count != 0) return; /* drop if buffer full */
            sockets[i].remote_ip = src_ip;
            sockets[i].remote_port = src_port;
            uint16_t copy_len = payload_len;
            if (copy_len > UDP_RX_BUFSZ) copy_len = UDP_RX_BUFSZ;
            memcpy(sockets[i].rx_buf, payload, copy_len);
            sockets[i].rx_count = copy_len;
            return;
        }
    }
}

udp_socket_t* udp_socket_create(void) {
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            sockets[i].used = 1;
            sockets[i].bound = 0;
            sockets[i].rx_count = 0;
            sockets[i].local_port = 0;
            return &sockets[i];
        }
    }
    return NULL;
}

void udp_socket_close(udp_socket_t* sock) {
    if (sock) sock->used = 0;
}

int udp_bind(udp_socket_t* sock, uint16_t port) {
    if (!sock || !sock->used) return -1;
    sock->local_port = port;
    sock->bound = 1;
    return 0;
}

int udp_sendto(udp_socket_t* sock, uint32_t dst_ip, uint16_t dst_port,
               const uint8_t* data, uint16_t len) {
    if (!sock || !sock->used) return -1;

    uint8_t packet[NET_MAX_PACKET];
    uint16_t total = sizeof(udp_header_t) + len;
    if (total > NET_MAX_PACKET) return -1;

    udp_header_t* hdr = (udp_header_t*)packet;
    hdr->src_port = (sock->local_port >> 8) | ((sock->local_port & 0xFF) << 8);
    hdr->dst_port = (dst_port >> 8) | ((dst_port & 0xFF) << 8);
    hdr->len      = (total >> 8) | ((total & 0xFF) << 8);
    hdr->checksum = 0;

    memcpy(packet + sizeof(udp_header_t), data, len);
    return ipv4_transmit(&g_netif, dst_ip, IP_PROTO_UDP, packet, total);
}

int udp_recvfrom(udp_socket_t* sock, uint8_t* buf, uint16_t maxlen,
                 uint32_t* out_src_ip, uint16_t* out_src_port,
                 uint32_t timeout_ms) {
    if (!sock || !sock->used) return -1;

    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (sock->rx_count > 0) {
            uint16_t copy_len = sock->rx_count;
            if (copy_len > maxlen) copy_len = maxlen;
            memcpy(buf, sock->rx_buf, copy_len);
            if (out_src_ip) *out_src_ip = sock->remote_ip;
            if (out_src_port) *out_src_port = sock->remote_port;
            sock->rx_count = 0;
            return (int)copy_len;
        }
        pit_delay_ms(10);
        waited += 10;
    }
    return -1;
}

const udp_socket_t* udp_get_sockets(int* count) {
    *count = UDP_MAX_SOCKETS;
    return sockets;
}
