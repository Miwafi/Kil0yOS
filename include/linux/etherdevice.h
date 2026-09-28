#ifndef _COMPAT_LINUX_ETHERDEVICE_H
#define _COMPAT_LINUX_ETHERDEVICE_H
#include "linux/netdevice.h"
#include "linux/slab.h"   /* errno values */

static inline int is_valid_ether_addr(const __u8* a) {
    return !(a[0] & 1) && (a[0] | a[1] | a[2] | a[3] | a[4] | a[5]);
}
static inline void eth_random_addr(__u8* addr) {
    for (int i = 0; i < ETH_ALEN; i++)
        addr[i] = (__u8)(0xA0 + i);   /* deterministic placeholder */
    addr[0] &= 0xFE;
    addr[0] |= 0x02;
}
#define eth_hw_addr_random(dev) eth_random_addr((dev)->dev_addr)

static inline void eth_hw_addr_set(struct net_device* dev, const __u8* addr) {
    memcpy(dev->dev_addr, addr, ETH_ALEN);
}

static inline int eth_validate_addr(struct net_device* dev) {
    return is_valid_ether_addr(dev->dev_addr) ? 0 : -EINVAL;
}

/* Ethernet FCS CRC (reflected poly 0xEDB88320), as used by the
 * 64-entry multicast hash filter: top 6 bits index MAR0..7. */
static inline __u32 ether_crc(int length, const unsigned char* data) {
    __u32 crc = 0xffffffff;
    while (--length >= 0) {
        unsigned char c = *data++;
        for (int i = 8; i > 0; i--) {
            if ((crc ^ c) & 1)
                crc = (crc >> 1) ^ 0xedb88320u;
            else
                crc >>= 1;
            c >>= 1;
        }
    }
    return crc;
}

/* alloc_etherdev: fixed "eth0" — the compat layer supports a single
 * netif; broadcast/promisc defaults come from alloc_netdev itself. */
static inline void ether_setup(struct net_device* dev) { (void)dev; }
static inline struct net_device* alloc_etherdev(int sizeof_priv) {
    return alloc_netdev(sizeof_priv, "eth0", ether_setup);
}

#endif
