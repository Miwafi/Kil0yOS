#ifndef UDP_H
#define UDP_H

#include "lib/types.h"
#include "net/netif.h"

#define UDP_MAX_SOCKETS 8
#define UDP_RX_BUFSZ    2048

typedef struct __attribute__((packed)) udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} udp_header_t;

typedef struct udp_socket {
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    int      bound;
    int      used;

    uint8_t  rx_buf[UDP_RX_BUFSZ];
    uint16_t rx_head;
    uint16_t rx_tail;
    uint16_t rx_count;
} udp_socket_t;

void udp_init(void);
void udp_receive(netif_t* iface, uint32_t src_ip, const uint8_t* data, uint16_t len);

udp_socket_t* udp_socket_create(void);
void          udp_socket_close(udp_socket_t* sock);
int           udp_bind(udp_socket_t* sock, uint16_t port);
int           udp_sendto(udp_socket_t* sock, uint32_t dst_ip, uint16_t dst_port,
                         const uint8_t* data, uint16_t len);
int           udp_recvfrom(udp_socket_t* sock, uint8_t* buf, uint16_t maxlen,
                           uint32_t* out_src_ip, uint16_t* out_src_port,
                           uint32_t timeout_ms);
const udp_socket_t* udp_get_sockets(int* count);

#endif
