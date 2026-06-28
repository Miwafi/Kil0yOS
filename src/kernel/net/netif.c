#include "net/netif.h"
#include "net/ethernet.h"
#include "net/rtl8139.h"
#include "net/e1000.h"
#include "drivers/pci.h"
#include "drivers/vga.h"

netif_t g_netif;

int netif_init(void) {
    g_netif.flags = 0;
    g_netif.tx_pending = 0;
    g_netif.send = NULL;
    g_netif.poll = NULL;
    return 0;
}

void netif_poll(void) {
    if (g_netif.poll) g_netif.poll();
}

void netif_receive(const uint8_t* data, uint16_t len) {
    if (len < ETH_HDR_LEN) return;
    eth_receive(&g_netif, data, len);
}

int netif_send(const uint8_t* data, uint16_t len) {
    if (g_netif.send == NULL) return -1;
    return g_netif.send(data, len);
}

int netif_tx_ready(void) {
    if (g_netif.send == NULL) return 0;
    return 1;
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
        if (dev->class_code == 0x02 && dev->subclass_code == 0x00) {
            if (dev->vendor_id == 0x10EC && dev->device_id == 0x8139) {
                if (rtl8139_init() == 0) return "RTL8139";
            } else if (dev->vendor_id == 0x8086 && dev->device_id == 0x100E) {
                if (e1000_init() == 0) return "E1000";
            }
        }
        dev = dev->next;
    }
    return NULL;
}
