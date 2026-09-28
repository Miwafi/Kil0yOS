/*
 * linux/ethtool.h - minimal ethtool surface
 *
 * Only the structures and ops-vector the driver fills in. There is no
 * ethtool ioctl consumer in this kernel, so the callbacks exist to make
 * driver code link and to keep register-dump paths honest.
 */
#ifndef _COMPAT_LINUX_ETHTOOL_H
#define _COMPAT_LINUX_ETHTOOL_H

#include "linux/types.h"

struct net_device;

#define ETH_GSTRING_LEN 32
#define ETH_SS_STATS    0

/* Wake-on-LAN flags */
#define WAKE_PHY    (1 << 0)
#define WAKE_UCAST  (1 << 1)
#define WAKE_MCAST  (1 << 2)
#define WAKE_BCAST  (1 << 3)
#define WAKE_ARP    (1 << 4)
#define WAKE_MAGIC  (1 << 5)

struct ethtool_drvinfo {
    char driver[32];
    char version[32];
    char fw_version[32];
    char bus_info[32];
};

struct ethtool_wolinfo {
    __u32 cmd;
    __u32 supported;
    __u32 wolopts;
    __u32 sopass[3];
};

struct ethtool_regs {
    __u32 cmd;
    __u32 version;
    __u32 len;
};

struct ethtool_stats {
    __u32 cmd;
    __u32 n_stats;
};

struct ethtool_link_ksettings {
    __u32 cmd;
    struct {
        __u32 speed;
        __u8  duplex;
        __u8  autoneg;
        __u8  port;
    } base;
};

struct ethtool_ops {
    void (*get_drvinfo)(struct net_device*, struct ethtool_drvinfo*);
    int  (*get_regs_len)(struct net_device*);
    void (*get_regs)(struct net_device*, struct ethtool_regs*, void*);
    int  (*nway_reset)(struct net_device*);
    __u32 (*get_link)(struct net_device*);
    __u32 (*get_msglevel)(struct net_device*);
    void (*set_msglevel)(struct net_device*, __u32);
    void (*get_wol)(struct net_device*, struct ethtool_wolinfo*);
    int  (*set_wol)(struct net_device*, struct ethtool_wolinfo*);
    void (*get_strings)(struct net_device*, __u32 stringset, __u8* data);
    int  (*get_sset_count)(struct net_device*, int sset);
    void (*get_ethtool_stats)(struct net_device*, struct ethtool_stats*, __u64*);
    int  (*get_link_ksettings)(struct net_device*, struct ethtool_link_ksettings*);
    int  (*set_link_ksettings)(struct net_device*, const struct ethtool_link_ksettings*);
};

#endif /* _COMPAT_LINUX_ETHTOOL_H */
