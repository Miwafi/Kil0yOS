#ifndef TCP_H
#define TCP_H

#include "lib/types.h"
#include "net/netif.h"

#define TCP_MAX_SOCKETS 8
/* 64 KB: an HTTP RX burst delivers the whole response before the recv loop
 * drains anything (IRQ pushes segments back-to-back), so the ring must hold
 * the largest burst or tcp_rx_push silently drops bytes mid-stream
 * (observed: 14 KB .deb downloaded with SHA256 mismatch over the 8 KB ring). */
#define TCP_RX_BUFSZ    (64 * 1024)
#define TCP_MSS         1400   /* conservative: MTU 1500 - 40 hdr bytes */
#define TCP_RETRIES     8
#define TCP_RTO_MS      300    /* fixed retransmission timeout */

/* Connection states (subset for active-open clients) */
#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_FIN_WAIT1   3
#define TCP_FIN_WAIT2   4
#define TCP_TIME_WAIT   5
#define TCP_CLOSE_WAIT  6
#define TCP_LAST_ACK    7
#define TCP_CLOSING     8

/* TCP header flags */
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

typedef struct __attribute__((packed)) tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;   /* high nibble: header len in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_header_t;

typedef struct tcp_socket {
    int      used;
    /* state/tx_* are written from RX (IRQ/poll) context while syscall-side
     * waiting loops spin on them - they MUST be volatile or -O2 caches the
     * old value in a register (observed: connect kept retransmitting SYN
     * after ESTABLISHED until its 10 s timeout). */
    volatile int      state;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;

    /* send side */
    uint32_t iss;
    uint32_t snd_una;      /* lowest unacknowledged byte */
    uint32_t snd_nxt;      /* next byte to send */
    uint8_t  tx_seg[TCP_MSS];   /* unacknowledged segment (single-slot) */
    volatile uint16_t tx_len;
    uint8_t  tx_flags;
    volatile uint64_t tx_time_us;   /* when the segment was (re)sent, 0 = none */
    int      tx_retries;
    uint8_t  fin_queued;   /* FIN included in the current segment */

    /* receive side */
    uint32_t rcv_nxt;
    uint8_t  rx_buf[TCP_RX_BUFSZ];
    volatile uint16_t rx_head, rx_tail;  /* ring; empty when equal */
    uint8_t  rx_fin;       /* peer FIN received */
} tcp_socket_t;

void tcp_init(void);
void tcp_receive(netif_t* iface, uint32_t src_ip, const uint8_t* data, uint16_t len);

tcp_socket_t* tcp_socket_create(void);
void          tcp_socket_close(tcp_socket_t* sock);
int           tcp_bind(tcp_socket_t* sock, uint16_t port);
int           tcp_connect(tcp_socket_t* sock, uint32_t ip, uint16_t port);
int           tcp_send(tcp_socket_t* sock, const uint8_t* data, uint16_t len);
int           tcp_recv(tcp_socket_t* sock, uint8_t* buf, uint16_t maxlen,
                       uint32_t timeout_ms);
int           tcp_close(tcp_socket_t* sock);
void          tcp_poll(tcp_socket_t* sock);   /* retransmit timer */
int           tcp_rx_ready(tcp_socket_t* sock);

#endif
