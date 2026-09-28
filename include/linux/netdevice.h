/*
 * linux/netdevice.h + skbuff.h - minimal network device model
 *
 * net_device carries what a NIC driver needs; register_netdev() binds
 * its xmit path to the kernel's global netif and dev_kfree_skb drains
 * the ring on completion. netif_rx() feeds a received skb straight
 * into the existing protocol stack (netif_receive).
 */
#ifndef _COMPAT_LINUX_NETDEVICE_H
#define _COMPAT_LINUX_NETDEVICE_H

#include "net/netif.h"
#include "linux/types.h"

#define ETH_ALEN  6
#define ETH_HLEN  14
#define ETH_DATA_LEN 1500
#define NET_IP_ALIGN 2

struct sk_buff;

struct net_device_ops {
    int  (*ndo_open)(struct net_device* dev);
    int  (*ndo_stop)(struct net_device* dev);
    int  (*ndo_start_xmit)(struct sk_buff* skb, struct net_device* dev);
    void (*ndo_set_rx_mode)(struct net_device* dev);
};

struct net_device_stats {
    unsigned long rx_packets, tx_packets;
    unsigned long rx_bytes, tx_bytes;
    unsigned long rx_errors, tx_errors;
    unsigned long rx_dropped, tx_dropped;
};

struct net_device {
    char name[16];
    __u8 dev_addr[ETH_ALEN];
    __u8 broadcast[ETH_ALEN];
    unsigned long mem_start, mem_end;
    unsigned long base_addr;
    int  irq;
    void* priv;
    const struct net_device_ops* netdev_ops;
    void (*poll_hook)(struct net_device*);   /* compat extension: rx drain */
    struct net_device_stats stats;
    unsigned long state;
    unsigned long features;
    int  flags;
};

struct sk_buff {
    struct net_device* dev;
    __u8*  head;
    __u8*  data;
    __u32  len;
    __u32  tail_alloc;      /* total buffer size */
};

#define NETDEV_TX_OK  0
#define NETDEV_TX_BUSY 1

/* device flags (subset of Linux IFF_*) */
#define IFF_UP      0x1
#define IFF_RUNNING 0x40

struct net_device* alloc_netdev(int sizeof_priv, const char* name,
                                void (*setup)(struct net_device*));
void free_netdev(struct net_device* dev);
int  register_netdev(struct net_device* dev);
void unregister_netdev(struct net_device* dev);

#define SET_NETDEV_DEV(ndev, pdev) ((void)0)
#define netdev_priv(dev) ((dev)->priv)
#define dev_net(dev) ((void)0)

/* sk_buff API (header/data area, like Linux) */
struct sk_buff* compat_alloc_skb(unsigned int size);
void compat_kfree_skb(struct sk_buff* skb);
/* Linux semantics: skb->data stays at the frame head; skb_put grows
 * the used length and returns the previous end-of-data (tail). */
static inline __u8* skb_put(struct sk_buff* skb, unsigned int len) {
    __u8* old = skb->data + skb->len;
    skb->len += len;
    return old;
}
static inline void skb_reserve(struct sk_buff* skb, int len) {
    skb->data += len;
}
#define alloc_skb(size) compat_alloc_skb(size)
#define dev_kfree_skb(skb) compat_kfree_skb(skb)
#define dev_kfree_skb_any(skb) compat_kfree_skb(skb)
#define dev_kfree_skb_irq(skb) compat_kfree_skb(skb)
#define consume_skb(skb) compat_kfree_skb(skb)
#define kfree_skb(skb) compat_kfree_skb(skb)

int netif_rx(struct sk_buff* skb);

extern void* compat_netdev_priv;       /* defined in compat.c */
struct net_device* compat_g_netdev(void);

#define netif_carrier_on(dev)  ((dev)->flags |= IFF_RUNNING)
#define netif_carrier_off(dev) ((dev)->flags &= ~IFF_RUNNING)
#define netif_start_queue(dev) ((void)0)
#define netif_stop_queue(dev)  ((void)0)
#define netif_wake_queue(dev)  ((void)0)
#define netif_running(dev)     ((dev)->flags & IFF_RUNNING)
#define netif_queue_stopped(dev) 0

void eth_broadcast_addr(__u8* addr);

#endif /* _COMPAT_LINUX_NETDEVICE_H */
