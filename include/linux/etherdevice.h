#ifndef _COMPAT_LINUX_ETHERDEVICE_H
#define _COMPAT_LINUX_ETHERDEVICE_H
#include "linux/netdevice.h"

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

#endif
