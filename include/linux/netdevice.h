/*
 * linux/netdevice.h + skbuff.h - network device model
 *
 * net_device carries what a NIC driver needs; register_netdev() binds
 * its xmit path to the kernel's global netif, netif_rx()/netif_receive_
 * skb() feed received skbs straight into the existing protocol stack.
 *
 * NAPI runs in synchronous mode: __napi_schedule() invokes the poll
 * callback immediately from the interrupt context and the main-loop
 * poll_hook drains as a safety net (the 8139 IMR is silently cleared
 * on this platform, so interrupt-only RX is not reliable).
 */
#ifndef _COMPAT_LINUX_NETDEVICE_H
#define _COMPAT_LINUX_NETDEVICE_H

#include "net/netif.h"
#include "linux/types.h"
#include "linux/ethtool.h"
#include "lib/string.h"

#define ETH_ALEN  6
#define ETH_HLEN  14
#define ETH_ZLEN  60
#define ETH_DATA_LEN 1500
#define ETH_MIN_MTU  68
#define ETH_FCS_LEN  4
#define NET_IP_ALIGN 2

typedef int netdev_tx_t;
typedef unsigned long netdev_features_t;

#define NETIF_F_SG       (1 << 0)
#define NETIF_F_HW_CSUM  (1 << 2)
#define NETIF_F_HIGHDMA  (1 << 5)
#define NETIF_F_RXALL    (1 << 11)
#define NETIF_F_RXFCS    (1 << 12)

/* device flags (subset of Linux IFF_*) */
#define IFF_UP       0x1
#define IFF_BROADCAST 0x2
#define IFF_PROMISC  0x100
#define IFF_ALLMULTI 0x200
#define IFF_RUNNING  0x40

/* message-level debugging flags (netif_msg_*) */
#define NETIF_MSG_DRV    0
#define NETIF_MSG_PROBE  1
#define NETIF_MSG_LINK   2
#define NETIF_MSG_TIMER  3
#define NETIF_MSG_IFDOWN 4
#define NETIF_MSG_IFUP   5
#define NETIF_MSG_RX_ERR 6
#define NETIF_MSG_TX_ERR 7
#define NETIF_MSG_TX_QUEUED 8
#define NETIF_MSG_INTR   9
#define NETIF_MSG_TX_DONE 10
#define NETIF_MSG_RX_STATUS 11

struct sk_buff;
struct ifreq;
struct rtnl_link_stats64;

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

struct net_device_ops {
    int  (*ndo_open)(struct net_device* dev);
    int  (*ndo_stop)(struct net_device* dev);
    void (*ndo_get_stats64)(struct net_device* dev, struct rtnl_link_stats64* stats);
    int  (*ndo_validate_addr)(struct net_device* dev);
    int  (*ndo_set_mac_address)(struct net_device* dev, void* p);
    netdev_tx_t (*ndo_start_xmit)(struct sk_buff* skb, struct net_device* dev);
    void (*ndo_set_rx_mode)(struct net_device* dev);
    int  (*ndo_eth_ioctl)(struct net_device* dev, struct ifreq* rq, int cmd);
    void (*ndo_tx_timeout)(struct net_device* dev, unsigned int txqueue);
    int  (*ndo_set_features)(struct net_device* dev, netdev_features_t features);
};

struct net_device_stats {
    unsigned long rx_packets, tx_packets;
    unsigned long rx_bytes, tx_bytes;
    unsigned long rx_errors, tx_errors;
    unsigned long rx_dropped, tx_dropped;
    unsigned long multicast;
    unsigned long collisions;
    unsigned long rx_length_errors, rx_over_errors, rx_crc_errors;
    unsigned long rx_frame_errors, rx_fifo_errors, rx_missed_errors;
    unsigned long tx_aborted_errors, tx_carrier_errors;
    unsigned long tx_fifo_errors, tx_heartbeat_errors, tx_window_errors;
};

/* rtnl_link_stats64: first fields must mirror net_device_stats order
 * (netdev_stats_to_stats64 memcpy's the common prefix). */
struct rtnl_link_stats64 {
    unsigned long rx_packets, tx_packets;
    unsigned long rx_bytes, tx_bytes;
    unsigned long rx_errors, tx_errors;
    unsigned long rx_dropped, tx_dropped;
    unsigned long multicast, collisions;
    unsigned long rx_length_errors, rx_over_errors, rx_crc_errors;
    unsigned long rx_frame_errors, rx_fifo_errors, rx_missed_errors;
    unsigned long tx_aborted_errors, tx_carrier_errors;
    unsigned long tx_fifo_errors, tx_heartbeat_errors, tx_window_errors;
};

struct netdev_hw_addr {
    __u8 addr[ETH_ALEN];
    struct netdev_hw_addr* next;
};

struct napi_struct {
    struct net_device* dev;
    int (*poll)(struct napi_struct*, int budget);
    int state;
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
    const struct ethtool_ops* ethtool_ops;
    void (*poll_hook)(struct net_device*);   /* compat extension: main-loop drain */
    struct net_device_stats stats;
    netdev_features_t features;
    netdev_features_t hw_features;
    netdev_features_t vlan_features;
    unsigned int min_mtu, max_mtu;
    unsigned int watchdog_timeo;
    struct netdev_hw_addr* mc_list;
    int mc_count;
    unsigned long state;
    int  flags;
};

struct sk_buff {
    struct net_device* dev;
    __u8*  head;
    __u8*  data;
    __u32  len;
    __u32  tail_alloc;      /* total buffer size */
    __u16  protocol;
};

#define NETDEV_TX_OK  0
#define NETDEV_TX_BUSY 1

#define HAVE_ALLOC_NETDEV
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
static inline void skb_copy_to_linear_data(struct sk_buff* skb,
                                           const void* from, unsigned int len) {
    memcpy(skb->data, from, len);
}
static inline void skb_copy_to_linear_data_offset(struct sk_buff* skb,
                                                  unsigned int offset,
                                                  const void* from, unsigned int len) {
    memcpy(skb->data + offset, from, len);
}
/* No checksum offload in this stack: plain copy. */
static inline void skb_copy_and_csum_dev(const struct sk_buff* skb, __u8* to) {
    memcpy(to, skb->data, skb->len);
}
#define alloc_skb(size) compat_alloc_skb(size)
#define dev_kfree_skb(skb) compat_kfree_skb(skb)
#define dev_kfree_skb_any(skb) compat_kfree_skb(skb)
#define dev_kfree_skb_irq(skb) compat_kfree_skb(skb)
#define consume_skb(skb) compat_kfree_skb(skb)
#define kfree_skb(skb) compat_kfree_skb(skb)

int netif_rx(struct sk_buff* skb);
static inline int netif_receive_skb(struct sk_buff* skb) { return netif_rx(skb); }

/* Read the ethertype out of the frame head; the protocol stack parses
 * the frame itself, skb->protocol is informational. */
static inline __u16 eth_type_trans(struct sk_buff* skb, struct net_device* dev) {
    (void)dev;
    skb->protocol = (__u16)((skb->data[12] << 8) | skb->data[13]);
    return skb->protocol;
}

extern void* compat_netdev_priv;       /* defined in compat.c */
struct net_device* compat_g_netdev(void);

#define netif_carrier_on(dev)  ((dev)->flags |= IFF_RUNNING)
#define netif_carrier_off(dev) ((dev)->flags &= ~IFF_RUNNING)
#define netif_start_queue(dev) ((void)(dev))
#define netif_stop_queue(dev)  ((void)(dev))
#define netif_wake_queue(dev)  ((void)(dev))
#define netif_running(dev)     ((dev)->flags & IFF_RUNNING)
#define netif_queue_stopped(dev) 0
#define netif_device_attach(dev)   ((void)(dev))
#define netif_device_detach(dev)   ((void)(dev))

#define netdev_lock(dev)   ((void)(dev))
#define netdev_unlock(dev) ((void)(dev))

/* ---- NAPI (synchronous mode; impl in compat.c) ---- */
void netif_napi_add(struct net_device* dev, struct napi_struct* napi,
                    int (*poll)(struct napi_struct*, int));
static inline void napi_enable(struct napi_struct* n)      { (void)n; }
static inline void napi_disable(struct napi_struct* n)     { (void)n; }
static inline void napi_enable_locked(struct napi_struct* n) { (void)n; }
int  napi_schedule_prep(struct napi_struct* napi);
void __napi_schedule(struct napi_struct* napi);
int  napi_complete_done(struct napi_struct* napi, int work_done);
struct sk_buff* napi_alloc_skb(struct napi_struct* napi, unsigned int length);

/* ---- multicast list: none is ever registered, loops stay empty ---- */
#define netdev_mc_count(dev) ((dev)->mc_count)
#define netdev_for_each_mc_addr(ha, dev) \
    for ((ha) = (dev)->mc_list; (ha); (ha) = (ha)->next)

/* ---- stats bridge ---- */
static inline void netdev_stats_to_stats64(struct rtnl_link_stats64* stats64,
                                           const struct net_device_stats* netdev_stats) {
    memcpy(stats64, netdev_stats, sizeof(*netdev_stats));
}

/* ---- per-driver message gating (msg_enable member required) ---- */
#define netif_msg_drv(p)    ((p)->msg_enable & (1 << NETIF_MSG_DRV))
#define netif_msg_probe(p)  ((p)->msg_enable & (1 << NETIF_MSG_PROBE))
#define netif_msg_link(p)   ((p)->msg_enable & (1 << NETIF_MSG_LINK))
#define netif_msg_timer(p)  ((p)->msg_enable & (1 << NETIF_MSG_TIMER))
#define netif_msg_ifdown(p) ((p)->msg_enable & (1 << NETIF_MSG_IFDOWN))
#define netif_msg_ifup(p)   ((p)->msg_enable & (1 << NETIF_MSG_IFUP))
#define netif_msg_rx_err(p) ((p)->msg_enable & (1 << NETIF_MSG_RX_ERR))
#define netif_msg_tx_err(p) ((p)->msg_enable & (1 << NETIF_MSG_TX_ERR))
#define netif_msg_tx_queued(p) ((p)->msg_enable & (1 << NETIF_MSG_TX_QUEUED))
#define netif_msg_intr(p)   ((p)->msg_enable & (1 << NETIF_MSG_INTR))
#define netif_msg_tx_done(p) ((p)->msg_enable & (1 << NETIF_MSG_TX_DONE))
#define netif_msg_rx_status(p) ((p)->msg_enable & (1 << NETIF_MSG_RX_STATUS))

/* ---- printk wrappers (netdev_* keep device context in the message) ---- */
#define netdev_info(dev, fmt, ...)  printk(KERN_INFO fmt, ##__VA_ARGS__)
#define netdev_warn(dev, fmt, ...)  printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define netdev_err(dev, fmt, ...)   printk(KERN_ERR fmt, ##__VA_ARGS__)
#define netdev_notice(dev, fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define netdev_crit(dev, fmt, ...)  printk(KERN_ERR fmt, ##__VA_ARGS__)
/* verbose debug output is compiled out */
#define netdev_dbg(dev, fmt, ...)   do { (void)(dev); } while (0)
#define netdev_vdbg(dev, fmt, ...)  do { (void)(dev); } while (0)
#define netif_dbg(tp, grp, dev, fmt, ...) do { (void)(tp); (void)(dev); } while (0)
#define netif_info(tp, grp, dev, fmt, ...) \
    (netif_msg_##grp(tp) ? printk(KERN_INFO fmt, ##__VA_ARGS__) : 0)
#define netif_err(tp, grp, dev, fmt, ...) \
    (netif_msg_##grp(tp) ? printk(KERN_ERR fmt, ##__VA_ARGS__) : 0)
#define netif_warn(tp, grp, dev, fmt, ...) \
    (netif_msg_##grp(tp) ? printk(KERN_WARNING fmt, ##__VA_ARGS__) : 0)

void eth_broadcast_addr(__u8* addr);

/* linux/string.h helper: always NUL-terminates like the kernel version */
static inline int strscpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    if (n == 0) return -22;
    while (i + 1 < n && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return (int)i;
}

#endif /* _COMPAT_LINUX_NETDEVICE_H */
