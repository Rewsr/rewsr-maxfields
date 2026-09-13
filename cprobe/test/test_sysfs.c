/*
 * test_sysfs.c, exercises the rooted sysfs readers against a fixture
 * tree laid out exactly like a live /sys/class/net.
 */
#include <errno.h>
#include <string.h>

#include "../sysfs/rewsr_sysfs.h"
#include "ctest.h"
#include "tutil.h"

static char g_root[256];

static int build_net_fixture(void) {
    int rc = rewsr_tutil_mkroot(g_root, sizeof(g_root));
    if (rc != 0) {
        return rc;
    }
    /* eth0: healthy 100G port. */
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/eth0/mtu", "9000\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/eth0/ifindex", "2\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/eth0/speed",
                                 "100000\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/eth0/carrier", "1\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/eth0/type", "1\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/eth0/operstate",
                                 "up\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/eth0/address",
                                 "b8:ce:f6:11:22:33\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/eth0/duplex",
                                 "full\n");
    /* lo: loopback, no speed/duplex/carrier semantics. */
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/lo/mtu", "65536\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/lo/ifindex", "1\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/lo/type", "772\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/lo/operstate",
                                 "unknown\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/lo/address",
                                 "00:00:00:00:00:00\n");
    /* down0: link down, kernel reports speed -1 and unreadable
     * carrier (we simulate the EINVAL by omitting the file). */
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/down0/mtu",
                                 "1500\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/down0/ifindex",
                                 "3\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/down0/speed",
                                 "-1\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/down0/operstate",
                                 "down\n");
    /* bonding_masters is a regular FILE directly in class/net and
     * must not be scanned as an interface. */
    rc |= rewsr_tutil_write_file(g_root, "sys/class/net/bonding_masters",
                                 "bond0\n");
    /* Assorted attributes for the scalar readers. */
    rc |= rewsr_tutil_write_file(g_root, "sys/misc/hex_attr", "0x8086\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/misc/dec_attr", "1250000\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/misc/neg_attr", "-1\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/misc/junk_attr", "12abc\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/misc/empty_attr", "");
    rc |= rewsr_tutil_write_file(g_root, "sys/misc/multiline",
                                 "first line\nsecond line\n");
    rc |= rewsr_tutil_write_file(g_root, "sys/misc/no_newline", "bare");
    return rc;
}

static void test_read_first_line(void) {
    char buf[64];
    ASSERT_EQ_INT(0, rewsr_read_first_line(g_root, "sys/misc/multiline",
                                           buf, sizeof(buf)));
    ASSERT_EQ_STR("first line", buf);

    ASSERT_EQ_INT(0, rewsr_read_first_line(g_root, "sys/misc/no_newline",
                                           buf, sizeof(buf)));
    ASSERT_EQ_STR("bare", buf);

    ASSERT_EQ_INT(0, rewsr_read_first_line(g_root, "sys/misc/empty_attr",
                                           buf, sizeof(buf)));
    ASSERT_EQ_STR("", buf);

    ASSERT_EQ_INT(-ENOENT, rewsr_read_first_line(g_root, "sys/misc/nope",
                                                 buf, sizeof(buf)));

    /* Line longer than the buffer must refuse, not truncate. */
    char tiny[4];
    ASSERT_EQ_INT(-EMSGSIZE,
                  rewsr_read_first_line(g_root, "sys/misc/multiline",
                                        tiny, sizeof(tiny)));
}

static void test_read_u64(void) {
    uint64_t v = 0;
    ASSERT_EQ_INT(0, rewsr_sysfs_read_u64(g_root, "sys/misc/hex_attr", &v));
    ASSERT_EQ_U64(0x8086, v);

    ASSERT_EQ_INT(0, rewsr_sysfs_read_u64(g_root, "sys/misc/dec_attr", &v));
    ASSERT_EQ_U64(1250000, v);

    ASSERT_EQ_INT(-EINVAL,
                  rewsr_sysfs_read_u64(g_root, "sys/misc/neg_attr", &v));
    ASSERT_EQ_INT(-EINVAL,
                  rewsr_sysfs_read_u64(g_root, "sys/misc/junk_attr", &v));
    ASSERT_EQ_INT(-EINVAL,
                  rewsr_sysfs_read_u64(g_root, "sys/misc/empty_attr", &v));
    ASSERT_EQ_INT(-ENOENT,
                  rewsr_sysfs_read_u64(g_root, "sys/misc/nope", &v));
}

static void test_read_int(void) {
    long long v = 0;
    ASSERT_EQ_INT(0, rewsr_read_int(g_root, "sys/misc/neg_attr", &v));
    ASSERT_EQ_INT(-1, v);

    ASSERT_EQ_INT(0, rewsr_read_int(g_root, "sys/misc/dec_attr", &v));
    ASSERT_EQ_INT(1250000, v);

    ASSERT_EQ_INT(-EINVAL,
                  rewsr_read_int(g_root, "sys/misc/junk_attr", &v));
}

static void test_cpulist_basic(void) {
    uint64_t map[4];
    uint32_t count = 0;
    ASSERT_EQ_INT(0, rewsr_parse_cpulist("0-3,8-11", map, 4, &count));
    ASSERT_EQ_U64(8, count);
    ASSERT_EQ_U64(0x0F0Full, map[0]);
    ASSERT_EQ_U64(0, map[1]);
    ASSERT_TRUE(rewsr_cpumask_test(map, 4, 0));
    ASSERT_TRUE(rewsr_cpumask_test(map, 4, 11));
    ASSERT_TRUE(!rewsr_cpumask_test(map, 4, 4));
    ASSERT_TRUE(!rewsr_cpumask_test(map, 4, 500));
}

static void test_cpulist_singletons_and_newline(void) {
    uint64_t map[4];
    uint32_t count = 0;
    ASSERT_EQ_INT(0, rewsr_parse_cpulist("5\n", map, 4, &count));
    ASSERT_EQ_U64(1, count);
    ASSERT_EQ_U64(1ull << 5, map[0]);

    ASSERT_EQ_INT(0, rewsr_parse_cpulist("1,3,5,7\n", map, 4, &count));
    ASSERT_EQ_U64(4, count);
    ASSERT_EQ_U64(0xAAull, map[0]);
}

static void test_cpulist_word_spanning(void) {
    /* 0-127 fills two full words; a dual-socket EPYC posts exactly
     * this in cpu/online. */
    uint64_t map[2];
    uint32_t count = 0;
    ASSERT_EQ_INT(0, rewsr_parse_cpulist("0-127", map, 2, &count));
    ASSERT_EQ_U64(128, count);
    ASSERT_EQ_U64(UINT64_MAX, map[0]);
    ASSERT_EQ_U64(UINT64_MAX, map[1]);

    /* Range straddling the word boundary. */
    ASSERT_EQ_INT(0, rewsr_parse_cpulist("60-67", map, 2, &count));
    ASSERT_EQ_U64(8, count);
    ASSERT_EQ_U64(0xF000000000000000ull, map[0]);
    ASSERT_EQ_U64(0x0Full, map[1]);
}

static void test_cpulist_empty_and_overlap(void) {
    uint64_t map[1];
    uint32_t count = 99;
    /* Offline mask on a fully-online box is an empty string. */
    ASSERT_EQ_INT(0, rewsr_parse_cpulist("", map, 1, &count));
    ASSERT_EQ_U64(0, count);
    ASSERT_EQ_INT(0, rewsr_parse_cpulist("\n", map, 1, &count));
    ASSERT_EQ_U64(0, count);

    /* Overlapping ranges count distinct CPUs once. */
    ASSERT_EQ_INT(0, rewsr_parse_cpulist("0-3,2-5", map, 1, &count));
    ASSERT_EQ_U64(6, count);
}

static void test_cpulist_malformed(void) {
    uint64_t map[1];
    uint32_t count;
    ASSERT_EQ_INT(-EINVAL, rewsr_parse_cpulist("5-3", map, 1, &count));
    ASSERT_EQ_INT(-EINVAL, rewsr_parse_cpulist("abc", map, 1, &count));
    ASSERT_EQ_INT(-EINVAL, rewsr_parse_cpulist("1,,3", map, 1, &count));
    ASSERT_EQ_INT(-EINVAL, rewsr_parse_cpulist("1,", map, 1, &count));
    ASSERT_EQ_INT(-EINVAL, rewsr_parse_cpulist("1-", map, 1, &count));
    ASSERT_EQ_INT(-ERANGE, rewsr_parse_cpulist("64", map, 1, &count));
    ASSERT_EQ_INT(-ERANGE, rewsr_parse_cpulist("0-64", map, 1, &count));
    ASSERT_EQ_INT(-EINVAL, rewsr_parse_cpulist(NULL, map, 1, &count));
    ASSERT_EQ_INT(-EINVAL, rewsr_parse_cpulist("0", NULL, 1, &count));
}

static void test_net_scan(void) {
    struct rewsr_netdev devs[8];
    size_t found = 0;
    ASSERT_EQ_INT(0, rewsr_sysfs_scan_net(g_root, devs, 8, &found));
    /* bonding_masters (a file) must not count. */
    ASSERT_EQ_U64(3, found);

    /* Sorted: down0, eth0, lo. */
    ASSERT_EQ_STR("down0", devs[0].name);
    ASSERT_EQ_STR("eth0", devs[1].name);
    ASSERT_EQ_STR("lo", devs[2].name);

    ASSERT_EQ_INT(9000, devs[1].mtu);
    ASSERT_EQ_INT(2, devs[1].ifindex);
    ASSERT_EQ_INT(100000, devs[1].speed_mbps);
    ASSERT_EQ_INT(1, devs[1].carrier);
    ASSERT_EQ_INT(1, devs[1].arphrd_type);
    ASSERT_EQ_STR("up", devs[1].operstate);
    ASSERT_EQ_STR("b8:ce:f6:11:22:33", devs[1].address);
    ASSERT_EQ_STR("full", devs[1].duplex);

    /* Missing attributes surface as -1 / empty, never garbage. */
    ASSERT_EQ_INT(-1, devs[0].carrier);
    ASSERT_EQ_INT(-1, devs[0].speed_mbps);
    ASSERT_EQ_STR("", devs[0].duplex);
    ASSERT_EQ_INT(772, devs[2].arphrd_type);
    ASSERT_EQ_INT(-1, devs[2].speed_mbps);
}

static void test_net_scan_truncation(void) {
    struct rewsr_netdev devs[2];
    size_t found = 0;
    ASSERT_EQ_INT(0, rewsr_sysfs_scan_net(g_root, devs, 2, &found));
    /* found reports the real total so callers can detect the clip. */
    ASSERT_EQ_U64(3, found);
}

static void test_net_scan_missing_root(void) {
    size_t found = 123;
    ASSERT_EQ_INT(-ENOENT,
                  rewsr_sysfs_scan_net("/nonexistent-rewsr", NULL, 0,
                                       &found));
    ASSERT_EQ_U64(0, found);
}

REWSR_TEST_MAIN(
    if (build_net_fixture() != 0) {
        fprintf(stderr, "fixture setup failed\n");
        return 1;
    }
    RUN_TEST(test_read_first_line);
    RUN_TEST(test_read_u64);
    RUN_TEST(test_read_int);
    RUN_TEST(test_cpulist_basic);
    RUN_TEST(test_cpulist_singletons_and_newline);
    RUN_TEST(test_cpulist_word_spanning);
    RUN_TEST(test_cpulist_empty_and_overlap);
    RUN_TEST(test_cpulist_malformed);
    RUN_TEST(test_net_scan);
    RUN_TEST(test_net_scan_truncation);
    RUN_TEST(test_net_scan_missing_root);
    rewsr_tutil_rmtree(g_root);
)
