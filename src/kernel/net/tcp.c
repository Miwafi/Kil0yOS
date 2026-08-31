#include "net/tcp.h"
#include "net/ipv4.h"
#include "net/netif.h"
#include "timer/pit.h"
#include "core/interrupts.h"
#include "lib/string.h"
#include "lib/stdlib.h"

static tcp_socket_t sockets[TCP_MAX_SOCKETS];

/* ISN source: cheap xorshift seeded from uptime (process.c keeps its own) */
static uint32_t isn_state = 0;

static uint32_t tcp_isn(void) {
    if (isn_state == 0) isn_state = (uint32_t)pit_uptime_us() | 1;
    isn_state ^= isn_state << 13;
    isn_state ^= isn_state >> 17;
    isn_state ^= isn_state << 5;
    return isn_state;
}

static inline uint16_t tcp16(uint16_t v) { return net_htons(v); }
static inline uint32_t tcp32(uint32_t v) { return net_htonl(v); }

void tcp_init(void) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        sockets[i].used = 0;
        sockets[i].state = TCP_CLOSED;
    }
}

tcp_socket_t* tcp_socket_create(void) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            tcp_socket_t* s = &sockets[i];
            memset(s, 0, sizeof(*s));
            s->used = 1;
            s->state = TCP_CLOSED;
            return s;
        }
    }
    return NULL;
}

void tcp_socket_close(tcp_socket_t* sock) {
    if (sock) {
        sock->used = 0;
        sock->state = TCP_CLOSED;
    }
}

int tcp_bind(tcp_socket_t* sock, uint16_t port) {
    if (!sock || !sock->used) return -1;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (&sockets[i] != sock && sockets[i].used && sockets[i].local_port == port) {
            return -1;
        }
    }
    sock->local_port = port;
    return 0;
}

/* --- wire helpers ------------------------------------------------------- */

/* TCP checksum over the pseudo header (IPv4) + segment. The pseudo
 * header MUST be fed as bytes (network byte order) through the same
 * word-summation as the segment - mixing host-order 16-bit halves with
 * byte-stream words produces a wrong checksum and SLIRP silently drops
 * every segment (observed: SYN retransmitting, no SYN-ACK). */
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                             const uint8_t* seg, uint16_t seg_len) {
    uint8_t ph[12];
    /* src_ip/dst_ip are host-order values whose big-endian byte form is
     * exactly the wire representation (e.g. 10.0.2.15 -> 0x0A00020F ->
     * bytes 0A 00 02 0F). Do NOT run them through net_htonl first - that
     * double-swaps and corrupts the pseudo header. */
    ph[0] = (uint8_t)(src_ip >> 24); ph[1] = (uint8_t)(src_ip >> 16);
    ph[2] = (uint8_t)(src_ip >> 8);  ph[3] = (uint8_t)src_ip;
    ph[4] = (uint8_t)(dst_ip >> 24); ph[5] = (uint8_t)(dst_ip >> 16);
    ph[6] = (uint8_t)(dst_ip >> 8);  ph[7] = (uint8_t)dst_ip;
    ph[8] = 0; ph[9] = IP_PROTO_TCP;
    ph[10] = (uint8_t)(seg_len >> 8); ph[11] = (uint8_t)seg_len;

    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2)
        sum += ((uint16_t)ph[i] | ((uint16_t)ph[i + 1] << 8));
    for (uint16_t i = 0; i < seg_len; i += 2) {
        uint16_t word = seg[i];
        if (i + 1 < seg_len) word |= ((uint16_t)seg[i + 1] << 8);
        sum += word;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* Send one segment. payload/len may be 0 (pure SYN/ACK/FIN). */
static int tcp_transmit_seg(tcp_socket_t* sock, uint32_t seq, uint8_t flags,
                            const uint8_t* payload, uint16_t len) {
    uint8_t packet[NET_MAX_PACKET];
    uint16_t hdr_len = sizeof(tcp_header_t) + 4;   /* fixed + 4-byte MSS opt */
    uint16_t total = hdr_len + len;
    if (total > NET_MAX_PACKET - sizeof(ipv4_header_t)) return -1;

    tcp_header_t* h = (tcp_header_t*)packet;
    h->src_port = tcp16(sock->local_port);
    h->dst_port = tcp16(sock->remote_port);
    h->seq = tcp32(seq);
    h->ack = tcp32(sock->rcv_nxt);
    h->data_off = (uint8_t)((hdr_len / 4) << 4);
    h->flags = flags;
    /* Window = currently FREE ring bytes (not a constant): advertising a
     * fixed window lets the peer overrun the ring during a fast burst and
     * made tcp_rx_push drop stream bytes mid-download (SHA256 mismatch). */
    uint16_t used = (uint16_t)((sock->rx_head - sock->rx_tail) % TCP_RX_BUFSZ);
    uint16_t win = TCP_RX_BUFSZ - 1 - used;
    h->window = tcp16(win);
    h->urgent = 0;
    h->checksum = 0;

    uint8_t* opt = packet + sizeof(tcp_header_t);
    opt[0] = 2; opt[1] = 4;   /* MSS */
    opt[2] = (TCP_MSS >> 8) & 0xFF;
    opt[3] = TCP_MSS & 0xFF;

    if (len) memcpy(packet + hdr_len, payload, len);
    h->checksum = tcp_checksum(netif_get_ip(), sock->remote_ip, packet, total);

    int r = ipv4_transmit(&g_netif, sock->remote_ip, IP_PROTO_TCP, packet, total);
    if (r < 0) klog("[tcp] ipv4 tx fail\n");
    return r;
}

/* Push received stream bytes into the socket's ring buffer. Runs in RX
 * context; the syscall side only advances rx_tail, so the ring stays
 * consistent without locks (single producer, single consumer). */
static void tcp_rx_push(tcp_socket_t* sock, const uint8_t* data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        uint16_t next = (uint16_t)((sock->rx_head + 1) % TCP_RX_BUFSZ);
        if (next == sock->rx_tail) break;   /* full: drop the rest */
        sock->rx_buf[sock->rx_head] = data[i];
        sock->rx_head = next;
    }
}

/* --- RX state machine ---------------------------------------------------- */

void tcp_receive(netif_t* iface, uint32_t src_ip, const uint8_t* data, uint16_t len) {
    (void)iface;
    if (len < sizeof(tcp_header_t)) return;

    tcp_header_t h;
    memcpy(&h, data, sizeof(h));

    uint16_t src_port = net_ntohs(h.src_port);
    uint16_t dst_port = net_ntohs(h.dst_port);
    uint32_t seq = net_ntohl(h.seq);
    uint32_t ack = net_ntohl(h.ack);
    uint8_t flags = h.flags;

    /* NOTE: no per-packet klog here - this runs in the RTL8139 ISR and a
     * 3-line serial dump per segment (~10 ms at 115200 baud) stalls the
     * whole system during RX bursts and floods acceptance logs. */

    uint8_t off = (h.data_off >> 4) * 4;
    if (off < sizeof(tcp_header_t) || off > len) return;
    const uint8_t* payload = data + off;
    uint16_t payload_len = len - off;

    /* Verify the checksum; drop silently on mismatch. */
    tcp_socket_t* sock = NULL;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (sockets[i].used && sockets[i].local_port == dst_port &&
            sockets[i].remote_port == src_port &&
            (sockets[i].state != TCP_CLOSED) &&
            sockets[i].remote_ip == src_ip) {
            sock = &sockets[i];
            break;
        }
    }
    if (sock == NULL) return;

    /* Verify the checksum; drop silently on mismatch. */
    if (tcp_checksum(src_ip, netif_get_ip(), data, len) != 0) {
        klog("[tcp] rx cksum fail\n");
        return;
    }

    if (flags & TCP_FLAG_RST) {
        sock->state = TCP_CLOSED;
        return;
    }

    switch (sock->state) {
    case TCP_SYN_SENT:
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            if (ack != sock->iss + 1) return;   /* not for our SYN */
            sock->rcv_nxt = seq + 1;
            sock->snd_una = ack;
            sock->state = TCP_ESTABLISHED;
            /* Retire the SYN retransmit slot: leaving tx_time_us armed
             * makes tcp_connect fire a stray SYN after ESTABLISHED, which
             * SLIRP answers with RST and kills the fresh connection
             * (observed: "[tcp] send: state lost" right after the
             * handshake on the second HTTP request). */
            sock->tx_time_us = 0;
            sock->tx_retries = 0;
            sock->rcv_nxt += payload_len;       /* rare: SYN+ACK with data */
            tcp_rx_push(sock, payload, payload_len);
            tcp_transmit_seg(sock, sock->snd_nxt, TCP_FLAG_ACK, NULL, 0);
        }
        return;

    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT1:
    case TCP_FIN_WAIT2:
    case TCP_CLOSING:
        /* Process ACK: advance snd_una, retire the retransmit slot. */
        if (flags & TCP_FLAG_ACK) {
            int32_t fwd = (int32_t)(ack - sock->snd_una);
            if (fwd > 0 && (int32_t)(sock->snd_nxt - ack) >= 0) {
                sock->snd_una = ack;
                sock->tx_len = 0;
                sock->tx_time_us = 0;
                sock->fin_queued = 0;
                sock->tx_retries = 0;
            }
        }

        /* In-order data. */
        if (payload_len > 0) {
            if (seq == sock->rcv_nxt) {
                tcp_rx_push(sock, payload, payload_len);
                sock->rcv_nxt += payload_len;
            }
            /* Duplicate/out-of-order: dropped; the ACK below re-announces
             * rcv_nxt so the peer retransmits. */
            tcp_transmit_seg(sock, sock->snd_nxt, TCP_FLAG_ACK, NULL, 0);
        } else if (flags & TCP_FLAG_FIN) {
            /* FIN handled below; fall through without a stray ACK. */
        }

        if (flags & TCP_FLAG_FIN) {
            /* Monotonic advance only: a RETRANSMITED FIN carries no payload,
             * so seq + payload_len + 1 would REWIND rcv_nxt behind the data
             * already accepted - our ACKs would regress and the peer would
             * retransmit the FIN forever (observed: 6+ FIN|ACK retries). */
            uint32_t fin_end = seq + payload_len + 1;
            if (fin_end > sock->rcv_nxt) sock->rcv_nxt = fin_end;
            sock->rx_fin = 1;
            tcp_transmit_seg(sock, sock->snd_nxt, TCP_FLAG_ACK, NULL, 0);
            if (sock->state == TCP_ESTABLISHED) sock->state = TCP_CLOSE_WAIT;
            else if (sock->state == TCP_FIN_WAIT1) sock->state = TCP_CLOSING;
            else if (sock->state == TCP_FIN_WAIT2) sock->state = TCP_TIME_WAIT;
        }

        /* State transitions driven by our ACK having been received. */
        if (sock->state == TCP_FIN_WAIT1 && sock->tx_len == 0 && sock->fin_queued == 0) {
            sock->state = TCP_FIN_WAIT2;
        }
        if (sock->state == TCP_CLOSING && sock->tx_len == 0) {
            sock->state = TCP_TIME_WAIT;
        }
        return;

    case TCP_LAST_ACK:
        if ((flags & TCP_FLAG_ACK) && ack == sock->snd_nxt) {
            sock->state = TCP_CLOSED;
        }
        return;

    case TCP_TIME_WAIT:
        if (flags & TCP_FLAG_FIN) {
            tcp_transmit_seg(sock, sock->snd_nxt, TCP_FLAG_ACK, NULL, 0);
        }
        return;

    default:
        return;
    }
}

/* --- user-facing blocking API -------------------------------------------- */

/* Drive the retransmit timer; call repeatedly from waiting loops. */
void tcp_poll(tcp_socket_t* sock) {
    if (!sock || !sock->used || sock->tx_len == 0 || sock->tx_time_us == 0) return;
    uint64_t now = pit_uptime_us();
    if (now - sock->tx_time_us < (uint64_t)TCP_RTO_MS * 1000) return;

    if (sock->tx_retries >= TCP_RETRIES) {
        klog("[tcp] give up: peer gone\n");
        sock->state = TCP_CLOSED;   /* give up: peer is gone */
        sock->tx_time_us = 0;
        return;
    }
    tcp_transmit_seg(sock, sock->snd_una, sock->tx_flags, sock->tx_seg, sock->tx_len);
    sock->tx_time_us = pit_uptime_us();
    sock->tx_retries++;
}

/* Queue and (re)send the current segment, arming the retransmit timer. */
static void tcp_send_seg_tracked(tcp_socket_t* sock, uint8_t flags,
                                 const uint8_t* payload, uint16_t len) {
    if (len > TCP_MSS) len = TCP_MSS;
    memcpy(sock->tx_seg, payload, len);
    sock->tx_len = len;
    sock->tx_flags = flags | TCP_FLAG_ACK;
    sock->fin_queued = (flags & TCP_FLAG_FIN) != 0;
    sock->tx_retries = 0;
    tcp_transmit_seg(sock, sock->snd_una, sock->tx_flags, payload, len);
    sock->tx_time_us = pit_uptime_us();
}

int tcp_connect(tcp_socket_t* sock, uint32_t ip, uint16_t port) {
    if (!sock || !sock->used) return -1;
    if (sock->state != TCP_CLOSED) return -1;

    sock->remote_ip = ip;
    sock->remote_port = port;
    if (sock->local_port == 0) {
        /* ephemeral port; simple bump allocator */
        static uint16_t next = 49152;
        for (int tries = 0; tries < 0x4000; tries++) {
            uint16_t p = next++;
            int taken = 0;
            for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
                if (&sockets[i] != sock && sockets[i].used &&
                    sockets[i].local_port == p) { taken = 1; break; }
            }
            if (!taken) { sock->local_port = p; break; }
            if (next < 49152) next = 49152;
        }
        if (sock->local_port == 0) {
            klog("[tcp] connect: no ephemeral port free\n");
            return -1;
        }
    }

    sock->iss = tcp_isn();
    sock->snd_una = sock->iss;
    sock->snd_nxt = sock->iss + 1;
    sock->rcv_nxt = 0;
    sock->rx_head = sock->rx_tail = 0;
    sock->rx_fin = 0;
    sock->state = TCP_SYN_SENT;

    tcp_transmit_seg(sock, sock->iss, TCP_FLAG_SYN, NULL, 0);
    sock->tx_len = 0;
    sock->tx_flags = TCP_FLAG_SYN;
    sock->fin_queued = 0;
    sock->tx_retries = 0;
    sock->tx_time_us = pit_uptime_us();

    /* Exponential backoff for SYN retransmission: a fixed 300 ms RTO
     * burns the retry budget in 2.4 s, far too aggressive for real
     * internet paths (one lost SYN to a mirror then killed the connect).
     * 500 ms doubling to a 4 s cap fits 4-5 SYNs into the 10 s window. */
    uint64_t rto_us = 500000;
    uint64_t start = pit_uptime_us();
    while (sock->state == TCP_SYN_SENT) {
        netif_poll();
        /* The poll above may have completed the handshake; retransmitting
         * the SYN against an ESTABLISHED SLIRP socket earns an RST. */
        if (sock->state != TCP_SYN_SENT) break;
        /* SYN retransmit (tx slot is unused for the SYN itself) */
        if (sock->tx_time_us != 0 &&
            pit_uptime_us() - sock->tx_time_us > rto_us) {
            if (sock->tx_retries >= TCP_RETRIES) {
                sock->state = TCP_CLOSED;
                klog("[tcp] connect: SYN retries exhausted\n");
                break;
            }
            tcp_transmit_seg(sock, sock->iss, TCP_FLAG_SYN, NULL, 0);
            sock->tx_time_us = pit_uptime_us();
            sock->tx_retries++;
            if (rto_us < 4000000ULL) rto_us *= 2;
        }
        if (pit_uptime_us() - start > 10000000ULL) {
            klog("[tcp] connect: 10s timeout, no SYN-ACK (retx=");
            char nb[8]; itoa(sock->tx_retries, nb, 10, sizeof(nb));
            klog(nb); klog(")\n");
            return -1;  /* 10 s */
        }
        pit_delay_ms(2);
    }
    sock->tx_time_us = 0;
    sock->tx_retries = 0;   /* SYN retries must not eat the data-segment budget */
    if (sock->state != TCP_ESTABLISHED) klog("[tcp] connect failed\n");
    return sock->state == TCP_ESTABLISHED ? 0 : -1;
}

int tcp_send(tcp_socket_t* sock, const uint8_t* data, uint16_t len) {
    if (!sock || !sock->used || sock->state != TCP_ESTABLISHED) {
        /* diagnostic: which of the three conditions rejected the send */
        klog("[tcp] send reject: used=");
        klog_hex("", sock && sock->used ? 1 : 0);
        klog_hex(" state=", sock ? (uint32_t)sock->state : 0xFFFFFFFF);
        return -1;
    }

    uint16_t sent = 0;
    while (sent < len) {
        uint16_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        /* Advance snd_nxt BEFORE transmitting: the RTL8139 RX path is
         * interrupt-driven and SLIRP (host-side) answers within
         * microseconds - the peer's ACK can be processed between the
         * transmit and a post-send update, judging the ACK stale
         * (snd_nxt - ack < 0) and never retiring tx_len
         * (observed: "[tcp] send: state lost" on the 2nd HTTP request). */
        sock->snd_nxt += chunk;
        tcp_send_seg_tracked(sock, TCP_FLAG_ACK | TCP_FLAG_PSH,
                             data + sent, chunk);
        sent += chunk;

        /* Wait for the ACK (or the peer's FIN closing the link). The ACK of
         * our data often arrives in the same RX burst as the peer's FIN
         * (HTTP/1.0 servers respond then close immediately) - so success
         * is tx_len == 0, NOT the socket still being ESTABLISHED. */
        uint64_t start = pit_uptime_us();
        while (sock->tx_len != 0 && sock->state == TCP_ESTABLISHED) {
            netif_poll();
            tcp_poll(sock);
            if (sock->tx_len == 0) break;   /* our data was ACKed */
            if (sock->state != TCP_ESTABLISHED) break;
            if (pit_uptime_us() - start > 15000000ULL) { klog("[tcp] send: ack timeout\n"); return -1; }  /* 15 s */
            pit_delay_ms(2);
        }
        if (sock->tx_len != 0) { klog("[tcp] send: state lost\n"); return -1; }
    }
    return sent;
}

int tcp_rx_ready(tcp_socket_t* sock) {
    if (!sock || !sock->used) return 0;
    return sock->rx_head != sock->rx_tail || sock->rx_fin;
}

int tcp_recv(tcp_socket_t* sock, uint8_t* buf, uint16_t maxlen,
             uint32_t timeout_ms) {
    if (!sock || !sock->used) return -1;

    uint32_t waited = 0;
    while (1) {
        netif_poll();
        if (sock->state == TCP_CLOSED &&
            sock->rx_head == sock->rx_tail) return -1;

        if (sock->rx_head != sock->rx_tail) {
            uint16_t n = 0;
            while (n < maxlen && sock->rx_tail != sock->rx_head) {
                buf[n++] = sock->rx_buf[sock->rx_tail];
                sock->rx_tail = (uint16_t)((sock->rx_tail + 1) % TCP_RX_BUFSZ);
            }
            /* Window update: draining freed ring space the peer may be
             * waiting on (its sends stall once our advertised window
             * closes). Re-ACK so a flow-controlled peer resumes. */
            tcp_transmit_seg(sock, sock->snd_nxt, TCP_FLAG_ACK, NULL, 0);
            return n;
        }
        if (sock->rx_fin) return 0;   /* orderly EOF */
        if (timeout_ms != 0xFFFFFFFF && waited >= timeout_ms) return -1;
        pit_delay_ms(2);
        waited += 2;
    }
}

int tcp_close(tcp_socket_t* sock) {
    if (!sock || !sock->used) return -1;
    if (sock->state == TCP_CLOSED) {
        sock->used = 0;
        return 0;
    }

    if (sock->state == TCP_CLOSE_WAIT) {
        /* Peer already sent its FIN: reply with ours (LAST_ACK). */
        sock->state = TCP_LAST_ACK;
        sock->tx_flags = TCP_FLAG_ACK | TCP_FLAG_FIN;
        sock->tx_retries = 0;
        /* Advance snd_nxt BEFORE the transmit (IRQ race, see tcp_send). */
        sock->snd_nxt += 1;
        tcp_transmit_seg(sock, sock->snd_nxt - 1, sock->tx_flags, NULL, 0);
        sock->tx_time_us = pit_uptime_us();
        uint64_t start = pit_uptime_us();
        while (sock->state == TCP_LAST_ACK) {
            netif_poll();
            if (sock->tx_time_us != 0 &&
                pit_uptime_us() - sock->tx_time_us > (uint64_t)TCP_RTO_MS * 1000) {
                if (sock->tx_retries >= TCP_RETRIES) break;
                tcp_transmit_seg(sock, sock->snd_una, sock->tx_flags, NULL, 0);
                sock->tx_time_us = pit_uptime_us();
                sock->tx_retries++;
            }
            if (pit_uptime_us() - start > 3000000ULL) break;   /* 3 s */
            pit_delay_ms(2);
        }
        sock->state = TCP_CLOSED;
        sock->used = 0;
        return 0;
    }

    if (sock->state == TCP_ESTABLISHED) {
        /* Send FIN (carrying any pending semantics of a quiet link).
         * Advance snd_nxt BEFORE the transmit - same IRQ race as tcp_send:
         * the FIN-ACK can arrive before a post-send update runs. */
        sock->tx_len = 1;   /* FIN occupies one sequence number */
        sock->tx_flags = TCP_FLAG_ACK | TCP_FLAG_FIN;
        sock->fin_queued = 1;
        sock->tx_retries = 0;
        sock->state = TCP_FIN_WAIT1;
        sock->snd_nxt += 1;
        tcp_transmit_seg(sock, sock->snd_nxt - 1, sock->tx_flags, NULL, 0);
        sock->tx_time_us = pit_uptime_us();

        uint64_t start = pit_uptime_us();
        while (sock->state != TCP_CLOSED && sock->state != TCP_TIME_WAIT) {
            netif_poll();
            /* FIN retransmit while unacknowledged */
            if (sock->tx_len != 0 && sock->tx_time_us != 0 &&
                pit_uptime_us() - sock->tx_time_us > (uint64_t)TCP_RTO_MS * 1000) {
                if (sock->tx_retries >= TCP_RETRIES) break;
                tcp_transmit_seg(sock, sock->snd_una, sock->tx_flags, NULL, 0);
                sock->tx_time_us = pit_uptime_us();
                sock->tx_retries++;
            }
            if (pit_uptime_us() - start > 8000000ULL) break;   /* 8 s */
            pit_delay_ms(2);
        }
    }

    sock->state = TCP_CLOSED;
    sock->used = 0;
    return 0;
}
