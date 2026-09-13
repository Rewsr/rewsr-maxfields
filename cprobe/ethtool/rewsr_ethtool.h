/*
 * rewsr_ethtool.h, NIC facts over the SIOCETHTOOL ioctl.
 *
 * Replaces shelling out to ethtool(8) for driver info, link settings
 * and channel counts. The ioctl operations only exist on Linux; the
 * name/formatting helpers at the bottom are pure lookup tables and
 * compile everywhere (rewsr_ethtool_names.c), which is also where the
 * tests live.
 *
 * Every constant borrowed from the kernel uapi is spelled here with a
 * REWSR_ prefix and annotated with its uapi name, so this header is
 * self-contained and builds on hosts without linux/ethtool.h.
 */
#ifndef REWSR_ETHTOOL_H
#define REWSR_ETHTOOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* uapi linux/sockios.h: SIOCETHTOOL */
#define REWSR_SIOCETHTOOL 0x8946

/* uapi linux/ethtool.h command numbers:
 *   ETHTOOL_GDRVINFO       0x00000003
 *   ETHTOOL_GCHANNELS      0x0000003c
 *   ETHTOOL_GLINKSETTINGS  0x0000004c */
#define REWSR_ETHTOOL_GDRVINFO      0x00000003u
#define REWSR_ETHTOOL_GCHANNELS     0x0000003cu
#define REWSR_ETHTOOL_GLINKSETTINGS 0x0000004cu

/* uapi linux/ethtool.h: SPEED_UNKNOWN is -1 cast to u32. */
#define REWSR_SPEED_UNKNOWN 0xFFFFFFFFu

/* uapi linux/ethtool.h duplex values: DUPLEX_HALF, DUPLEX_FULL,
 * DUPLEX_UNKNOWN. */
#define REWSR_DUPLEX_HALF    0x00
#define REWSR_DUPLEX_FULL    0x01
#define REWSR_DUPLEX_UNKNOWN 0xFF

/* uapi linux/ethtool.h port values: PORT_TP, PORT_AUI, PORT_MII,
 * PORT_FIBRE, PORT_BNC, PORT_DA, PORT_NONE, PORT_OTHER. */
#define REWSR_PORT_TP    0x00
#define REWSR_PORT_AUI   0x01
#define REWSR_PORT_MII   0x02
#define REWSR_PORT_FIBRE 0x03
#define REWSR_PORT_BNC   0x04
#define REWSR_PORT_DA    0x05
#define REWSR_PORT_NONE  0xEF
#define REWSR_PORT_OTHER 0xFF

/*
 * Storage for the variable-length link mode bitmaps. The kernel's
 * __ETHTOOL_LINK_MODE_MASK_NBITS is 102 as of 6.x (4 u32 words); 16
 * words of headroom outlives several more mask growths, and the
 * handshake tells us the real count at runtime.
 */
#define REWSR_ETHTOOL_MAX_LMODE_WORDS 16

struct rewsr_ethtool_drvinfo_out {
    /* All strings NUL-terminated here even though the kernel arrays
     * are not guaranteed to be. */
    char driver[33];
    char version[33];
    char fw_version[33];
    char bus_info[33];
    uint32_t n_stats;
    uint32_t n_priv_flags;
};

struct rewsr_ethtool_link {
    uint32_t speed_mbps; /* REWSR_SPEED_UNKNOWN when link is down */
    uint8_t duplex;      /* REWSR_DUPLEX_* */
    uint8_t port;        /* REWSR_PORT_* */
    uint8_t autoneg;     /* 1 = on */
    int nwords;          /* words valid in each mask below */
    uint32_t supported[REWSR_ETHTOOL_MAX_LMODE_WORDS];
    uint32_t advertising[REWSR_ETHTOOL_MAX_LMODE_WORDS];
    uint32_t lp_advertising[REWSR_ETHTOOL_MAX_LMODE_WORDS];
};

struct rewsr_ethtool_channels_out {
    uint32_t max_rx;
    uint32_t max_tx;
    uint32_t max_other;
    uint32_t max_combined;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t other_count;
    uint32_t combined_count;
};

/*
 * Linux-only probes. Return 0 on success, -ENOTSUP off Linux, and
 * negative errno straight from the kernel otherwise (-EOPNOTSUPP is
 * common: virtual interfaces implement none of these, and kernels
 * before 4.6 lack ETHTOOL_GLINKSETTINGS entirely; no legacy
 * ETHTOOL_GSET fallback here because the fleet floor is 5.x).
 */
int rewsr_ethtool_drvinfo(const char *ifname,
                          struct rewsr_ethtool_drvinfo_out *out);
int rewsr_ethtool_link_settings(const char *ifname,
                                struct rewsr_ethtool_link *out);
int rewsr_ethtool_channels(const char *ifname,
                           struct rewsr_ethtool_channels_out *out);

/*
 * Portable name helpers (rewsr_ethtool_names.c). Static strings, or
 * NULL when the value has no name; rewsr_ethtool_speed_format always
 * renders something ("25G", "1234M", "unknown").
 */
const char *rewsr_ethtool_speed_name(uint32_t speed_mbps);
size_t rewsr_ethtool_speed_format(uint32_t speed_mbps, char *buf,
                                  size_t cap);
const char *rewsr_ethtool_duplex_name(uint8_t duplex);
const char *rewsr_ethtool_port_name(uint8_t port);

/*
 * Link mode bit names, indexed by uapi
 * enum ethtool_link_mode_bit_indices (0 = 10baseT/Half ... 97 =
 * 800000baseVR8/Full). NULL for bits we do not know.
 */
const char *rewsr_ethtool_link_mode_name(unsigned bit);

/*
 * Render every set bit of a link mode mask as a space-separated list.
 * Known bits print their name, unknown-but-set bits print "bitN" so
 * new modes are visible instead of dropped. snprintf-style return of
 * the full length needed.
 */
size_t rewsr_ethtool_link_modes_string(const uint32_t *mask, int nwords,
                                       char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* REWSR_ETHTOOL_H */
