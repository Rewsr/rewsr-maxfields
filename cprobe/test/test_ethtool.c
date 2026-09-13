/*
 * test_ethtool.c, covers the portable name/format helpers fully and
 * pins the platform behavior of the ioctl paths (real probing needs a
 * live Linux NIC and is exercised on the fleet, not here).
 */
#include <errno.h>
#include <string.h>

#include "../ethtool/rewsr_ethtool.h"
#include "ctest.h"

static void test_speed_names(void) {
    ASSERT_EQ_STR("10M", rewsr_ethtool_speed_name(10));
    ASSERT_EQ_STR("100M", rewsr_ethtool_speed_name(100));
    ASSERT_EQ_STR("1G", rewsr_ethtool_speed_name(1000));
    ASSERT_EQ_STR("2.5G", rewsr_ethtool_speed_name(2500));
    ASSERT_EQ_STR("10G", rewsr_ethtool_speed_name(10000));
    ASSERT_EQ_STR("25G", rewsr_ethtool_speed_name(25000));
    ASSERT_EQ_STR("40G", rewsr_ethtool_speed_name(40000));
    ASSERT_EQ_STR("100G", rewsr_ethtool_speed_name(100000));
    ASSERT_EQ_STR("200G", rewsr_ethtool_speed_name(200000));
    ASSERT_EQ_STR("400G", rewsr_ethtool_speed_name(400000));
    ASSERT_EQ_STR("800G", rewsr_ethtool_speed_name(800000));
    ASSERT_TRUE(rewsr_ethtool_speed_name(1234) == NULL);
    ASSERT_TRUE(rewsr_ethtool_speed_name(0) == NULL);
}

static void test_speed_format(void) {
    char buf[32];
    rewsr_ethtool_speed_format(25000, buf, sizeof(buf));
    ASSERT_EQ_STR("25G", buf);
    rewsr_ethtool_speed_format(REWSR_SPEED_UNKNOWN, buf, sizeof(buf));
    ASSERT_EQ_STR("unknown", buf);
    rewsr_ethtool_speed_format(0, buf, sizeof(buf));
    ASSERT_EQ_STR("unknown", buf);
    /* Unmapped multiples of 1G render as G, others as M. */
    rewsr_ethtool_speed_format(3000, buf, sizeof(buf));
    ASSERT_EQ_STR("3G", buf);
    rewsr_ethtool_speed_format(1234, buf, sizeof(buf));
    ASSERT_EQ_STR("1234M", buf);
}

static void test_duplex_port_names(void) {
    ASSERT_EQ_STR("full", rewsr_ethtool_duplex_name(REWSR_DUPLEX_FULL));
    ASSERT_EQ_STR("half", rewsr_ethtool_duplex_name(REWSR_DUPLEX_HALF));
    ASSERT_EQ_STR("unknown",
                  rewsr_ethtool_duplex_name(REWSR_DUPLEX_UNKNOWN));
    ASSERT_TRUE(rewsr_ethtool_duplex_name(0x42) == NULL);

    ASSERT_EQ_STR("twisted-pair", rewsr_ethtool_port_name(REWSR_PORT_TP));
    ASSERT_EQ_STR("fibre", rewsr_ethtool_port_name(REWSR_PORT_FIBRE));
    ASSERT_EQ_STR("direct-attach", rewsr_ethtool_port_name(REWSR_PORT_DA));
    ASSERT_EQ_STR("none", rewsr_ethtool_port_name(REWSR_PORT_NONE));
    ASSERT_TRUE(rewsr_ethtool_port_name(0x42) == NULL);
}

static void test_link_mode_names(void) {
    ASSERT_EQ_STR("10baseT/Half", rewsr_ethtool_link_mode_name(0));
    ASSERT_EQ_STR("Autoneg", rewsr_ethtool_link_mode_name(6));
    ASSERT_EQ_STR("10000baseT/Full", rewsr_ethtool_link_mode_name(12));
    ASSERT_EQ_STR("25000baseCR/Full", rewsr_ethtool_link_mode_name(31));
    ASSERT_EQ_STR("100000baseCR4/Full", rewsr_ethtool_link_mode_name(38));
    ASSERT_EQ_STR("FEC_RS", rewsr_ethtool_link_mode_name(50));
    ASSERT_EQ_STR("200000baseCR4/Full", rewsr_ethtool_link_mode_name(66));
    ASSERT_EQ_STR("400000baseCR8/Full", rewsr_ethtool_link_mode_name(73));
    ASSERT_EQ_STR("800000baseVR8/Full", rewsr_ethtool_link_mode_name(97));
    ASSERT_TRUE(rewsr_ethtool_link_mode_name(98) == NULL);
    ASSERT_TRUE(rewsr_ethtool_link_mode_name(4096) == NULL);
}

static void test_link_modes_string(void) {
    /* A ConnectX-5 style supported mask: Autoneg + 25G CR + 100G CR4
     * + FEC_RS. Bits 6, 31, 38, 50. */
    uint32_t mask[2] = { 0, 0 };
    mask[0] = (1u << 6) | (1u << 31);
    mask[1] = (1u << (38 - 32)) | (1u << (50 - 32));

    char buf[128];
    size_t need = rewsr_ethtool_link_modes_string(mask, 2, buf,
                                                  sizeof(buf));
    ASSERT_EQ_STR("Autoneg 25000baseCR/Full 100000baseCR4/Full FEC_RS",
                  buf);
    ASSERT_EQ_U64(strlen(buf), need);
}

static void test_link_modes_string_unknown_bits(void) {
    /* A future kernel sets bit 99: it must show up as bit99, not
     * vanish. */
    uint32_t mask[4] = { 0, 0, 0, 0 };
    mask[3] = 1u << (99 - 96);
    char buf[64];
    rewsr_ethtool_link_modes_string(mask, 4, buf, sizeof(buf));
    ASSERT_EQ_STR("bit99", buf);
}

static void test_link_modes_string_truncation(void) {
    uint32_t mask[1] = { (1u << 6) | (1u << 13) }; /* Autoneg Pause */
    char tiny[9];
    size_t need = rewsr_ethtool_link_modes_string(mask, 1, tiny,
                                                  sizeof(tiny));
    /* "Autoneg Pause" needs 13; only "Autoneg" fits in 9. */
    ASSERT_EQ_STR("Autoneg", tiny);
    ASSERT_EQ_U64(13, need);

    ASSERT_EQ_U64(0, rewsr_ethtool_link_modes_string(NULL, 4, tiny,
                                                     sizeof(tiny)));
    ASSERT_EQ_STR("", tiny);
}

static void test_ioctl_platform_behavior(void) {
    struct rewsr_ethtool_drvinfo_out di;
    struct rewsr_ethtool_link ls;
    struct rewsr_ethtool_channels_out ch;
#if defined(__linux__)
    /* Loopback accepts the ioctl socket path even though it backs
     * none of these ops; every outcome must be a clean errno, and
     * garbage interface names must not reach the kernel. */
    ASSERT_EQ_INT(-EINVAL, rewsr_ethtool_drvinfo(NULL, &di));
    ASSERT_EQ_INT(-EINVAL, rewsr_ethtool_drvinfo("", &di));
    ASSERT_EQ_INT(-EINVAL,
                  rewsr_ethtool_drvinfo("interface-name-way-too-long",
                                        &di));
    int rc = rewsr_ethtool_link_settings("lo", &ls);
    ASSERT_TRUE(rc == 0 || rc == -EOPNOTSUPP);
    rc = rewsr_ethtool_channels("lo", &ch);
    ASSERT_TRUE(rc == 0 || rc == -EOPNOTSUPP);
#else
    ASSERT_EQ_INT(-ENOTSUP, rewsr_ethtool_drvinfo("en0", &di));
    ASSERT_EQ_INT(-ENOTSUP, rewsr_ethtool_link_settings("en0", &ls));
    ASSERT_EQ_INT(-ENOTSUP, rewsr_ethtool_channels("en0", &ch));
    /* The refusal still leaves defined values behind. */
    ASSERT_EQ_U64(REWSR_SPEED_UNKNOWN, ls.speed_mbps);
    ASSERT_EQ_INT(REWSR_DUPLEX_UNKNOWN, ls.duplex);
#endif
}

REWSR_TEST_MAIN(
    RUN_TEST(test_speed_names);
    RUN_TEST(test_speed_format);
    RUN_TEST(test_duplex_port_names);
    RUN_TEST(test_link_mode_names);
    RUN_TEST(test_link_modes_string);
    RUN_TEST(test_link_modes_string_unknown_bits);
    RUN_TEST(test_link_modes_string_truncation);
    RUN_TEST(test_ioctl_platform_behavior);
)
