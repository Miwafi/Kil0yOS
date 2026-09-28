/*
 * linux/mii.h - MII register interface
 *
 * Constants are verbatim Linux; the helper functions are faithful
 * reimplementations of the mii.c core the 8139 driver paths use
 * (nway result resolution, media check, link-ok, restart). The 8139
 * internal PHY maps MII regs 0/1/4/5/6 onto NWay registers, so
 * mdio_read/mdio_write callbacks do the real work.
 */
#ifndef _COMPAT_LINUX_MII_H
#define _COMPAT_LINUX_MII_H

#include "linux/types.h"
#include "linux/kernel.h"
#include "linux/netdevice.h"
#include "linux/ethtool.h"

/* Basic Mode Control Register */
#define MII_BMCR         0x00
#define BMCR_SPEED100    0x2000
#define BMCR_FULLDPLX    0x0100
#define BMCR_ANRESTART   0x0200
#define BMCR_RESET       0x8000
#define BMCR_ANENABLE    0x1000

/* Basic Mode Status Register */
#define MII_BMSR         0x01
#define BMSR_LSTACK      0x0004
#define BMSR_ANEGCAPABLE 0x0008
#define BMSR_ANEGCOMPLETE 0x0020

/* Auto-negotiation advertisement / link partner ability */
#define MII_ADVERTISE    0x04
#define MII_LPA          0x05
#define MII_EXPANSION    0x06

#define ADVERTISE_10HALF 0x0020
#define ADVERTISE_10FULL 0x0040
#define ADVERTISE_100HALF 0x0080
#define ADVERTISE_100FULL 0x0100
#define LPA_10HALF       ADVERTISE_10HALF
#define LPA_10FULL       ADVERTISE_10FULL
#define LPA_100HALF      ADVERTISE_100HALF
#define LPA_100FULL      ADVERTISE_100FULL

/* ethtool speed/duplex encodings */
#define SPEED_10   10
#define SPEED_100  100
#define DUPLEX_HALF 0x00
#define DUPLEX_FULL 0x01

struct mii_if_info {
    int phy_id;
    int advertising;
    int force_media;
    int full_duplex;
    struct net_device* dev;
    int  (*mdio_read)(struct net_device* dev, int phy_id, int location);
    void (*mdio_write)(struct net_device* dev, int phy_id, int location, int val);
    unsigned phy_id_mask;
    unsigned reg_num_mask;
};

/* Resolve an auto-negotiation result to the highest common ability. */
static inline unsigned int mii_nway_result(unsigned int negotiated) {
    unsigned int duplex;
    if (negotiated & LPA_100FULL)      duplex = LPA_100FULL;
    else if (negotiated & LPA_100HALF) duplex = LPA_100HALF;
    else if (negotiated & LPA_10FULL)  duplex = LPA_10FULL;
    else                               duplex = LPA_10HALF;
    return duplex;
}

/* Media check on the internal PHY. Returns 1 when the duplex setting
 * changed (the caller reprograms the MAC accordingly). */
static inline int mii_check_media(struct mii_if_info* mii,
                                  unsigned int ok_to_print,
                                  unsigned int init_media) {
    int advertise, lpa, media, duplex;
    (void)init_media;
    if (mii->force_media) return 0;
    advertise = mii->mdio_read(mii->dev, mii->phy_id, MII_ADVERTISE);
    lpa = mii->mdio_read(mii->dev, mii->phy_id, MII_LPA);
    media = mii_nway_result(lpa & advertise);
    duplex = (media & (ADVERTISE_100FULL | ADVERTISE_10FULL)) ? 1 : 0;
    if (duplex != mii->full_duplex) {
        mii->full_duplex = duplex;
        if (ok_to_print)
            printk(KERN_INFO "mii: %s: link %s-duplex\n",
                   mii->dev ? mii->dev->name : "?", duplex ? "full" : "half");
        return 1;
    }
    return 0;
}

/* Link status: BMSR read twice (latch-low clearing semantics). */
static inline u32 mii_link_ok(struct mii_if_info* mii) {
    if (!mii->mdio_read) return 0;
    mii->mdio_read(mii->dev, mii->phy_id, MII_BMSR);
    return (mii->mdio_read(mii->dev, mii->phy_id, MII_BMSR) & BMSR_LSTACK) ? 1 : 0;
}

static inline int mii_nway_restart(struct mii_if_info* mii) {
    int bmcr;
    if (!mii->mdio_read || !mii->mdio_write) return -EINVAL;
    bmcr = mii->mdio_read(mii->dev, mii->phy_id, MII_BMCR);
    if (!(bmcr & BMCR_ANENABLE)) return -EINVAL;
    mii->mdio_write(mii->dev, mii->phy_id, MII_BMCR, bmcr | BMCR_ANRESTART);
    return 0;
}

/* ---- ethtool ksettings bridges over the mii callbacks ---- */
static inline int mii_ethtool_get_link_ksettings(struct mii_if_info* mii,
                                                 struct ethtool_link_ksettings* cmd) {
    u32 supported = mii_link_ok(mii);
    int bmcr = mii->mdio_read(mii->dev, mii->phy_id, MII_BMCR);
    cmd->base.speed = (bmcr & BMCR_SPEED100) ? SPEED_100 : SPEED_10;
    cmd->base.duplex = (bmcr & BMCR_FULLDPLX) ? DUPLEX_FULL : DUPLEX_HALF;
    cmd->base.autoneg = (bmcr & BMCR_ANENABLE) ? 1 : 0;
    (void)supported;
    return 0;
}

static inline int mii_ethtool_set_link_ksettings(struct mii_if_info* mii,
                                                 const struct ethtool_link_ksettings* cmd) {
    int bmcr;
    if (cmd->base.autoneg) {
        bmcr = mii->mdio_read(mii->dev, mii->phy_id, MII_BMCR);
        mii->mdio_write(mii->dev, mii->phy_id, MII_BMCR,
                        bmcr | BMCR_ANENABLE | BMCR_ANRESTART);
        mii->force_media = 0;
    } else {
        bmcr = mii->mdio_read(mii->dev, mii->phy_id, MII_BMCR);
        bmcr &= ~(BMCR_ANENABLE | BMCR_SPEED100 | BMCR_FULLDPLX);
        if (cmd->base.speed == SPEED_100) bmcr |= BMCR_SPEED100;
        if (cmd->base.duplex == DUPLEX_FULL) bmcr |= BMCR_FULLDPLX;
        mii->mdio_write(mii->dev, mii->phy_id, MII_BMCR, bmcr);
        mii->force_media = 1;
        mii->full_duplex = (cmd->base.duplex == DUPLEX_FULL);
    }
    return 0;
}

/* ---- ioctl bridge (SIOCGMIIPHY family); no caller in this kernel ---- */
#define SIOCGMIIPHY  0x8947
#define SIOCGMIIREG  0x8948
#define SIOCSMIIREG  0x8949

struct mii_ioctl_data {
    __u16 phy_id;
    __u16 reg_num;
    __u16 val_in;
    __u16 val_out;
};

struct ifreq {
    union {
        char ifru_name[16];
        struct mii_ioctl_data ifru_mii;
    } ifr_ifru;
};
#define ifr_name  ifr_ifru.ifru_name
#define ifr_mii   ifr_ifru.ifru_mii

#define if_mii(rq) (&(rq)->ifr_mii)

static inline int generic_mii_ioctl(struct mii_if_info* mii,
                                    struct mii_ioctl_data* mii_data, int cmd,
                                    unsigned int* duplex_changed) {
    int rc = -EOPNOTSUPP;
    (void)mii; (void)mii_data; (void)cmd; (void)duplex_changed;
    return rc;
}

#endif /* _COMPAT_LINUX_MII_H */
