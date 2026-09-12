#include "net/netif.h"
#include "net/ethernet.h"
#include "net/rtl8139.h"
#include "net/e1000.h"
#include "drivers/pci.h"
#include "drivers/vga.h"

netif_t g_netif;

/* Set while a received frame is being dispatched. Frame dispatch can
 * transmit (ICMP echo reply, TCP ACK), which may trigger arp_resolve ->
 * netif_poll; draining new frames inside that dispatch recurses and each
 * level burns ~4KB of stack for packet buffers, overflowing the kernel
 * stack (observed as corrupted interrupt frames / #GP). */
static int rx_busy = 0;

int netif_poll_busy(void) {
    return rx_busy;
}

int netif_init(void) {
    g_netif.flags = 0;
    g_netif.send = NULL;
    g_netif.poll = NULL;
    return 0;
}

void netif_poll(void) {
    /* Nested poll (from a transmit inside frame dispatch): refuse. The
     * frames stay in the NIC ring and are drained by the next top-level
     * poll (main loop, socket wait, ...). */
    if (rx_busy) return;
    if (g_netif.poll) g_netif.poll();
}

void netif_receive(const uint8_t* data, uint16_t len) {
    if (len < ETH_HDR_LEN) return;
    /* Drivers must never hand us frames larger than the stack can hold:
     * upper layers memcpy len-sized payloads into fixed buffers. */
    if (len > NET_MAX_PACKET) return;
    rx_busy = 1;
    eth_receive(&g_netif, data, len);
    rx_busy = 0;
}

int netif_send(const uint8_t* data, uint16_t len) {
    if (g_netif.send == NULL) return -1;
    return g_netif.send(data, len);
}

void netif_get_mac(uint8_t* out_mac) {
    for (int i = 0; i < NET_MAC_LEN; i++) {
        out_mac[i] = g_netif.mac[i];
    }
}

uint32_t netif_get_ip(void) {
    return g_netif.ip;
}

const char* netif_probe(void) {
    pci_device_t* dev = pci_get_device_list();
    while (dev) {
        const char* drv = NULL;
        /* Match by VID:DID only - never by class. VMware's e1000 (82545EM)
         * reports class 0210 (subclass 0x10), not 0200, so a class filter
         * silently rejects a NIC we fully support. */
        if (dev->vendor_id == 0x10EC && dev->device_id == 0x8139) {
            if (rtl8139_init() == 0) drv = "RTL8139";
        } else if (dev->vendor_id == 0x8086 &&
                   (dev->device_id == 0x100E || dev->device_id == 0x100F ||
                    dev->device_id == 0x10D3 || dev->device_id == 0x10F6)) {
            if (e1000_init() == 0) drv = "E1000";
        }
        if (drv) return drv;

        /* Network-class device we could not drive: report its identity
         * so the user knows what is missing (e.g. vmxnet3). */
        if (dev->class_code == 0x02) {
            char buf[40];
            const char hex[] = "0123456789abcdef";
            char* p = buf;
            const char* s = "[net] unsupported NIC ";
            while (*s) *p++ = *s++;
            for (int i = 0; i < 4; i++) *p++ = hex[(dev->vendor_id >> (12 - i * 4)) & 0xF];
            *p++ = ':';
            for (int i = 0; i < 4; i++) *p++ = hex[(dev->device_id >> (12 - i * 4)) & 0xF];
            *p++ = '\n'; *p = 0;
            klog(buf);
        }
        dev = dev->next;
    }
    return NULL;
}
