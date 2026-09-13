/*
 * rewsr_ethtool_names.c, portable value-to-name tables for ethtool
 * facts. No syscalls; runs and tests anywhere.
 *
 * The link mode table is transcribed from uapi linux/ethtool.h
 * enum ethtool_link_mode_bit_indices (through bit 97, the 800G
 * modes added in 6.x). Names are rendered the way ethtool(8) prints
 * them so downstream fact consumers see familiar spellings.
 */
#include "rewsr_ethtool.h"

#include <stdio.h>
#include <string.h>

const char *rewsr_ethtool_speed_name(uint32_t speed_mbps) {
    switch (speed_mbps) {
    case 10:      return "10M";
    case 100:     return "100M";
    case 1000:    return "1G";
    case 2500:    return "2.5G";
    case 5000:    return "5G";
    case 10000:   return "10G";
    case 14000:   return "14G";   /* InfiniBand FDR per-lane rate */
    case 20000:   return "20G";
    case 25000:   return "25G";
    case 40000:   return "40G";
    case 50000:   return "50G";
    case 56000:   return "56G";
    case 100000:  return "100G";
    case 200000:  return "200G";
    case 400000:  return "400G";
    case 800000:  return "800G";
    default:      return NULL;
    }
}

size_t rewsr_ethtool_speed_format(uint32_t speed_mbps, char *buf,
                                  size_t cap) {
    if (speed_mbps == REWSR_SPEED_UNKNOWN || speed_mbps == 0) {
        return (size_t)snprintf(buf, cap, "unknown");
    }
    const char *name = rewsr_ethtool_speed_name(speed_mbps);
    if (name != NULL) {
        return (size_t)snprintf(buf, cap, "%s", name);
    }
    /* Oddball rates (some virtual drivers report whatever they like)
     * still render honestly. */
    if (speed_mbps % 1000 == 0) {
        return (size_t)snprintf(buf, cap, "%uG", speed_mbps / 1000);
    }
    return (size_t)snprintf(buf, cap, "%uM", speed_mbps);
}

const char *rewsr_ethtool_duplex_name(uint8_t duplex) {
    switch (duplex) {
    case REWSR_DUPLEX_HALF:    return "half";
    case REWSR_DUPLEX_FULL:    return "full";
    case REWSR_DUPLEX_UNKNOWN: return "unknown";
    default:                   return NULL;
    }
}

const char *rewsr_ethtool_port_name(uint8_t port) {
    switch (port) {
    case REWSR_PORT_TP:    return "twisted-pair";
    case REWSR_PORT_AUI:   return "aui";
    case REWSR_PORT_MII:   return "mii";
    case REWSR_PORT_FIBRE: return "fibre";
    case REWSR_PORT_BNC:   return "bnc";
    case REWSR_PORT_DA:    return "direct-attach";
    case REWSR_PORT_NONE:  return "none";
    case REWSR_PORT_OTHER: return "other";
    default:               return NULL;
    }
}

/* Indexed by uapi ETHTOOL_LINK_MODE_*_BIT value. */
static const char *const k_link_mode_names[] = {
    /*  0 */ "10baseT/Half",
    /*  1 */ "10baseT/Full",
    /*  2 */ "100baseT/Half",
    /*  3 */ "100baseT/Full",
    /*  4 */ "1000baseT/Half",
    /*  5 */ "1000baseT/Full",
    /*  6 */ "Autoneg",
    /*  7 */ "TP",
    /*  8 */ "AUI",
    /*  9 */ "MII",
    /* 10 */ "FIBRE",
    /* 11 */ "BNC",
    /* 12 */ "10000baseT/Full",
    /* 13 */ "Pause",
    /* 14 */ "Asym_Pause",
    /* 15 */ "2500baseX/Full",
    /* 16 */ "Backplane",
    /* 17 */ "1000baseKX/Full",
    /* 18 */ "10000baseKX4/Full",
    /* 19 */ "10000baseKR/Full",
    /* 20 */ "10000baseR_FEC",
    /* 21 */ "20000baseMLD2/Full",
    /* 22 */ "20000baseKR2/Full",
    /* 23 */ "40000baseKR4/Full",
    /* 24 */ "40000baseCR4/Full",
    /* 25 */ "40000baseSR4/Full",
    /* 26 */ "40000baseLR4/Full",
    /* 27 */ "56000baseKR4/Full",
    /* 28 */ "56000baseCR4/Full",
    /* 29 */ "56000baseSR4/Full",
    /* 30 */ "56000baseLR4/Full",
    /* 31 */ "25000baseCR/Full",
    /* 32 */ "25000baseKR/Full",
    /* 33 */ "25000baseSR/Full",
    /* 34 */ "50000baseCR2/Full",
    /* 35 */ "50000baseKR2/Full",
    /* 36 */ "100000baseKR4/Full",
    /* 37 */ "100000baseSR4/Full",
    /* 38 */ "100000baseCR4/Full",
    /* 39 */ "100000baseLR4_ER4/Full",
    /* 40 */ "50000baseSR2/Full",
    /* 41 */ "1000baseX/Full",
    /* 42 */ "10000baseCR/Full",
    /* 43 */ "10000baseSR/Full",
    /* 44 */ "10000baseLR/Full",
    /* 45 */ "10000baseLRM/Full",
    /* 46 */ "10000baseER/Full",
    /* 47 */ "2500baseT/Full",
    /* 48 */ "5000baseT/Full",
    /* 49 */ "FEC_NONE",
    /* 50 */ "FEC_RS",
    /* 51 */ "FEC_BASER",
    /* 52 */ "50000baseKR/Full",
    /* 53 */ "50000baseSR/Full",
    /* 54 */ "50000baseCR/Full",
    /* 55 */ "50000baseLR_ER_FR/Full",
    /* 56 */ "50000baseDR/Full",
    /* 57 */ "100000baseKR2/Full",
    /* 58 */ "100000baseSR2/Full",
    /* 59 */ "100000baseCR2/Full",
    /* 60 */ "100000baseLR2_ER2_FR2/Full",
    /* 61 */ "100000baseDR2/Full",
    /* 62 */ "200000baseKR4/Full",
    /* 63 */ "200000baseSR4/Full",
    /* 64 */ "200000baseLR4_ER4_FR4/Full",
    /* 65 */ "200000baseDR4/Full",
    /* 66 */ "200000baseCR4/Full",
    /* 67 */ "100baseT1/Full",
    /* 68 */ "1000baseT1/Full",
    /* 69 */ "400000baseKR8/Full",
    /* 70 */ "400000baseSR8/Full",
    /* 71 */ "400000baseLR8_ER8_FR8/Full",
    /* 72 */ "400000baseDR8/Full",
    /* 73 */ "400000baseCR8/Full",
    /* 74 */ "FEC_LLRS",
    /* 75 */ "100000baseKR/Full",
    /* 76 */ "100000baseSR/Full",
    /* 77 */ "100000baseLR_ER_FR/Full",
    /* 78 */ "100000baseCR/Full",
    /* 79 */ "100000baseDR/Full",
    /* 80 */ "200000baseKR2/Full",
    /* 81 */ "200000baseSR2/Full",
    /* 82 */ "200000baseLR2_ER2_FR2/Full",
    /* 83 */ "200000baseDR2/Full",
    /* 84 */ "200000baseCR2/Full",
    /* 85 */ "400000baseKR4/Full",
    /* 86 */ "400000baseSR4/Full",
    /* 87 */ "400000baseLR4_ER4_FR4/Full",
    /* 88 */ "400000baseDR4/Full",
    /* 89 */ "400000baseCR4/Full",
    /* 90 */ "100baseFX/Half",
    /* 91 */ "100baseFX/Full",
    /* 92 */ "800000baseCR8/Full",
    /* 93 */ "800000baseKR8/Full",
    /* 94 */ "800000baseDR8/Full",
    /* 95 */ "800000baseDR8_2/Full",
    /* 96 */ "800000baseSR8/Full",
    /* 97 */ "800000baseVR8/Full",
};

const char *rewsr_ethtool_link_mode_name(unsigned bit) {
    if (bit >= sizeof(k_link_mode_names) / sizeof(k_link_mode_names[0])) {
        return NULL;
    }
    return k_link_mode_names[bit];
}

size_t rewsr_ethtool_link_modes_string(const uint32_t *mask, int nwords,
                                       char *buf, size_t cap) {
    size_t need = 0;
    size_t used = 0;
    int first = 1;
    if (cap > 0) {
        buf[0] = '\0';
    }
    if (mask == NULL || nwords <= 0) {
        return 0;
    }
    for (int w = 0; w < nwords; w++) {
        for (int b = 0; b < 32; b++) {
            if (((mask[w] >> b) & 1u) == 0) {
                continue;
            }
            unsigned bit = (unsigned)(w * 32 + b);
            const char *name = rewsr_ethtool_link_mode_name(bit);
            char tmp[16];
            if (name == NULL) {
                /* New kernel bit this table has not learned yet:
                 * surface it rather than hiding capability. */
                snprintf(tmp, sizeof(tmp), "bit%u", bit);
                name = tmp;
            }
            size_t nlen = strlen(name);
            size_t add = nlen + (first ? 0 : 1);
            need += add;
            if (cap > 0 && used + add < cap) {
                if (!first) {
                    buf[used] = ' ';
                    memcpy(buf + used + 1, name, nlen);
                } else {
                    memcpy(buf + used, name, nlen);
                }
                used += add;
                buf[used] = '\0';
            }
            first = 0;
        }
    }
    return need;
}
