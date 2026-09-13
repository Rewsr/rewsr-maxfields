/*
 * rewsr_ethtool.c, the SIOCETHTOOL ioctl calls.
 *
 * The kernel struct layouts are copied verbatim from uapi
 * linux/ethtool.h (each is annotated with its uapi name) instead of
 * including the kernel header, so the file syntax-checks on hosts
 * that do not have it. The layouts are ABI, they cannot drift: the
 * ioctl numbers and struct sizes are the contract.
 */
#include "rewsr_ethtool.h"

#include <errno.h>
#include <string.h>

#if defined(__linux__)

#include <net/if.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * uapi struct ethtool_drvinfo. The three 32s after cmd are
 * driver/version/fw_version; ETHTOOL_FWVERS_LEN, ETHTOOL_BUSINFO_LEN
 * and ETHTOOL_EROMVERS_LEN are all 32.
 */
struct rewsr_uapi_ethtool_drvinfo {
    uint32_t cmd;
    char driver[32];
    char version[32];
    char fw_version[32];
    char bus_info[32];
    char erom_version[32];
    char reserved2[12];
    uint32_t n_priv_flags;
    uint32_t n_stats;
    uint32_t testinfo_len;
    uint32_t eedump_len;
    uint32_t regdump_len;
};

/* uapi struct ethtool_channels. */
struct rewsr_uapi_ethtool_channels {
    uint32_t cmd;
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
 * uapi struct ethtool_link_settings, fixed header only; the three
 * link mode bitmaps (supported, advertising, lp_advertising) follow
 * it back to back, each link_mode_masks_nwords u32 words long. The
 * master_slave and rate_matching bytes started life as reserved1[];
 * the header size (48 bytes before the flexible area) never changed.
 */
struct rewsr_uapi_ethtool_link_settings {
    uint32_t cmd;
    uint32_t speed;
    uint8_t duplex;
    uint8_t port;
    uint8_t phy_address;
    uint8_t autoneg;
    uint8_t mdio_support;
    uint8_t eth_tp_mdix;
    uint8_t eth_tp_mdix_ctrl;
    int8_t link_mode_masks_nwords;
    uint8_t transceiver;
    uint8_t master_slave_cfg;
    uint8_t master_slave_state;
    uint8_t rate_matching;
    uint32_t reserved[7];
    uint32_t link_mode_masks[];
};

/*
 * One ethtool ioctl round trip: open a throwaway AF_INET datagram
 * socket (the ethtool ioctls are routed off any socket, this is what
 * ethtool(8) itself uses), point ifr_data at the request, fire.
 */
static int ethtool_ioctl(const char *ifname, void *req) {
    if (ifname == NULL || ifname[0] == '\0' ||
        strlen(ifname) >= IFNAMSIZ) {
        return -EINVAL;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -errno;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    /* Length checked above, no truncation possible. */
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    ifr.ifr_data = req;

    int rc = 0;
    if (ioctl(fd, REWSR_SIOCETHTOOL, &ifr) != 0) {
        rc = -errno;
    }
    close(fd);
    return rc;
}

/* Kernel char arrays are not guaranteed NUL-terminated; bound-copy
 * into the one-larger caller field. */
static void copy_kstr(char *dst, size_t dcap, const char *src,
                      size_t scap) {
    size_t n = 0;
    while (n < scap && src[n] != '\0') {
        n++;
    }
    if (n >= dcap) {
        n = dcap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int rewsr_ethtool_drvinfo(const char *ifname,
                          struct rewsr_ethtool_drvinfo_out *out) {
    if (out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));

    struct rewsr_uapi_ethtool_drvinfo info;
    memset(&info, 0, sizeof(info));
    info.cmd = REWSR_ETHTOOL_GDRVINFO;

    int rc = ethtool_ioctl(ifname, &info);
    if (rc != 0) {
        return rc;
    }
    copy_kstr(out->driver, sizeof(out->driver), info.driver,
              sizeof(info.driver));
    copy_kstr(out->version, sizeof(out->version), info.version,
              sizeof(info.version));
    copy_kstr(out->fw_version, sizeof(out->fw_version), info.fw_version,
              sizeof(info.fw_version));
    copy_kstr(out->bus_info, sizeof(out->bus_info), info.bus_info,
              sizeof(info.bus_info));
    out->n_stats = info.n_stats;
    out->n_priv_flags = info.n_priv_flags;
    return 0;
}

int rewsr_ethtool_link_settings(const char *ifname,
                                struct rewsr_ethtool_link *out) {
    if (out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->speed_mbps = REWSR_SPEED_UNKNOWN;
    out->duplex = REWSR_DUPLEX_UNKNOWN;

    /* Backing store for header + 3 bitmaps at maximum width. */
    struct {
        struct rewsr_uapi_ethtool_link_settings s;
        uint32_t masks[3 * REWSR_ETHTOOL_MAX_LMODE_WORDS];
    } req;

    /*
     * The GLINKSETTINGS handshake, per the uapi header: pass nwords 0
     * first; the kernel refuses with the required word count NEGATED
     * in link_mode_masks_nwords, then the same call repeated with the
     * positive count succeeds and fills the three masks.
     */
    memset(&req, 0, sizeof(req));
    req.s.cmd = REWSR_ETHTOOL_GLINKSETTINGS;
    req.s.link_mode_masks_nwords = 0;

    int rc = ethtool_ioctl(ifname, &req);
    if (rc != 0) {
        return rc;
    }
    if (req.s.link_mode_masks_nwords >= 0) {
        /* Handshake contract violated; treat as protocol error rather
         * than trusting whatever came back. */
        return -EPROTO;
    }
    int nwords = -req.s.link_mode_masks_nwords;
    if (nwords > REWSR_ETHTOOL_MAX_LMODE_WORDS) {
        return -EOVERFLOW;
    }

    memset(&req, 0, sizeof(req));
    req.s.cmd = REWSR_ETHTOOL_GLINKSETTINGS;
    req.s.link_mode_masks_nwords = (int8_t)nwords;
    rc = ethtool_ioctl(ifname, &req);
    if (rc != 0) {
        return rc;
    }
    if (req.s.link_mode_masks_nwords != nwords) {
        return -EPROTO;
    }

    out->speed_mbps = req.s.speed;
    out->duplex = req.s.duplex;
    out->port = req.s.port;
    out->autoneg = req.s.autoneg;
    out->nwords = nwords;
    for (int i = 0; i < nwords; i++) {
        out->supported[i] = req.s.link_mode_masks[i];
        out->advertising[i] = req.s.link_mode_masks[nwords + i];
        out->lp_advertising[i] = req.s.link_mode_masks[2 * nwords + i];
    }
    return 0;
}

int rewsr_ethtool_channels(const char *ifname,
                           struct rewsr_ethtool_channels_out *out) {
    if (out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));

    struct rewsr_uapi_ethtool_channels ch;
    memset(&ch, 0, sizeof(ch));
    ch.cmd = REWSR_ETHTOOL_GCHANNELS;

    int rc = ethtool_ioctl(ifname, &ch);
    if (rc != 0) {
        return rc;
    }
    out->max_rx = ch.max_rx;
    out->max_tx = ch.max_tx;
    out->max_other = ch.max_other;
    out->max_combined = ch.max_combined;
    out->rx_count = ch.rx_count;
    out->tx_count = ch.tx_count;
    out->other_count = ch.other_count;
    out->combined_count = ch.combined_count;
    return 0;
}

#else /* !__linux__ */

/*
 * SIOCETHTOOL is a Linux ioctl; there is no equivalent to emulate.
 * The name helpers in rewsr_ethtool_names.c stay fully functional.
 */

int rewsr_ethtool_drvinfo(const char *ifname,
                          struct rewsr_ethtool_drvinfo_out *out) {
    (void)ifname;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return -ENOTSUP;
}

int rewsr_ethtool_link_settings(const char *ifname,
                                struct rewsr_ethtool_link *out) {
    (void)ifname;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->speed_mbps = REWSR_SPEED_UNKNOWN;
        out->duplex = REWSR_DUPLEX_UNKNOWN;
    }
    return -ENOTSUP;
}

int rewsr_ethtool_channels(const char *ifname,
                           struct rewsr_ethtool_channels_out *out) {
    (void)ifname;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return -ENOTSUP;
}

#endif /* __linux__ */
