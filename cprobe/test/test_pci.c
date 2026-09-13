/*
 * test_pci.c, modalias parsing, class/vendor naming, link-speed
 * mapping, and the rooted device scan against a fixture tree.
 *
 * Modalias samples are verbatim sysfs contents from real devices:
 * an Intel I350 1GbE NIC, a Mellanox ConnectX-5, and a Samsung PM983
 * NVMe drive.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../pci/rewsr_pci.h"
#include "ctest.h"
#include "tutil.h"

static void test_modalias_i350(void) {
    struct rewsr_pci_id id;
    ASSERT_EQ_INT(0, rewsr_pci_parse_modalias(
        "pci:v00008086d00001521sv00008086sd00000001bc02sc00i00", &id));
    ASSERT_EQ_U64(0x8086, id.vendor);
    ASSERT_EQ_U64(0x1521, id.device);
    ASSERT_EQ_U64(0x8086, id.subvendor);
    ASSERT_EQ_U64(0x0001, id.subdevice);
    ASSERT_EQ_INT(0x02, id.base_class);
    ASSERT_EQ_INT(0x00, id.subclass);
    ASSERT_EQ_INT(0x00, id.prog_if);
}

static void test_modalias_connectx5_with_newline(void) {
    struct rewsr_pci_id id;
    /* Raw file contents keep their newline; the parser must not
     * care. */
    ASSERT_EQ_INT(0, rewsr_pci_parse_modalias(
        "pci:v000015B3d00001017sv000015B3sd00000020bc02sc00i00\n", &id));
    ASSERT_EQ_U64(0x15B3, id.vendor);
    ASSERT_EQ_U64(0x1017, id.device);
    ASSERT_EQ_INT(0x02, id.base_class);
}

static void test_modalias_nvme_lowercase(void) {
    struct rewsr_pci_id id;
    ASSERT_EQ_INT(0, rewsr_pci_parse_modalias(
        "pci:v0000144dd0000a808sv0000144dsd0000a801bc01sc08i02", &id));
    ASSERT_EQ_U64(0x144D, id.vendor);
    ASSERT_EQ_U64(0xA808, id.device);
    ASSERT_EQ_INT(0x01, id.base_class);
    ASSERT_EQ_INT(0x08, id.subclass);
    ASSERT_EQ_INT(0x02, id.prog_if);
}

static void test_modalias_malformed(void) {
    struct rewsr_pci_id id;
    /* Wrong bus prefix. */
    ASSERT_EQ_INT(-EINVAL, rewsr_pci_parse_modalias(
        "usb:v8086p1521", &id));
    /* Truncated. */
    ASSERT_EQ_INT(-EINVAL, rewsr_pci_parse_modalias(
        "pci:v00008086d0000", &id));
    /* Non-hex digit inside a fixed-width field. */
    ASSERT_EQ_INT(-EINVAL, rewsr_pci_parse_modalias(
        "pci:v0000ZZ86d00001521sv00008086sd00000001bc02sc00i00", &id));
    /* Trailing garbage after a complete parse. */
    ASSERT_EQ_INT(-EINVAL, rewsr_pci_parse_modalias(
        "pci:v00008086d00001521sv00008086sd00000001bc02sc00i00X", &id));
    ASSERT_EQ_INT(-EINVAL, rewsr_pci_parse_modalias("", &id));
    ASSERT_EQ_INT(-EINVAL, rewsr_pci_parse_modalias(NULL, &id));
}

static void test_class_names(void) {
    ASSERT_EQ_STR("Ethernet controller", rewsr_pci_class_name(0x020000));
    ASSERT_EQ_STR("Non-Volatile memory controller",
                  rewsr_pci_class_name(0x010802));
    ASSERT_EQ_STR("VGA compatible controller",
                  rewsr_pci_class_name(0x030000));
    ASSERT_EQ_STR("3D controller", rewsr_pci_class_name(0x030200));
    ASSERT_EQ_STR("PCI bridge", rewsr_pci_class_name(0x060400));
    ASSERT_EQ_STR("Host bridge", rewsr_pci_class_name(0x060000));
    ASSERT_EQ_STR("USB controller", rewsr_pci_class_name(0x0C0330));
    ASSERT_EQ_STR("Infiniband controller", rewsr_pci_class_name(0x020700));
    ASSERT_EQ_STR("Processing accelerator",
                  rewsr_pci_class_name(0x120000));
    /* Unknown subclass falls back to the base-class name. */
    ASSERT_EQ_STR("Mass storage controller",
                  rewsr_pci_class_name(0x01F900));
    /* Unknown base class yields NULL, callers print the hex. */
    ASSERT_TRUE(rewsr_pci_class_name(0xEE0000) == NULL);
}

static void test_vendor_names(void) {
    ASSERT_EQ_STR("Intel", rewsr_pci_vendor_name(0x8086));
    ASSERT_EQ_STR("Mellanox Technologies", rewsr_pci_vendor_name(0x15B3));
    ASSERT_EQ_STR("NVIDIA", rewsr_pci_vendor_name(0x10DE));
    ASSERT_EQ_STR("Amazon Annapurna Labs", rewsr_pci_vendor_name(0x1D0F));
    ASSERT_EQ_STR("Samsung Electronics", rewsr_pci_vendor_name(0x144D));
    ASSERT_TRUE(rewsr_pci_vendor_name(0x0666) == NULL);
}

static void test_link_speed_mapping(void) {
    ASSERT_EQ_INT(1, rewsr_pci_gen_from_speed_str("2.5 GT/s PCIe"));
    ASSERT_EQ_INT(2, rewsr_pci_gen_from_speed_str("5.0 GT/s PCIe"));
    ASSERT_EQ_INT(3, rewsr_pci_gen_from_speed_str("8.0 GT/s PCIe"));
    ASSERT_EQ_INT(4, rewsr_pci_gen_from_speed_str("16.0 GT/s PCIe"));
    ASSERT_EQ_INT(5, rewsr_pci_gen_from_speed_str("32.0 GT/s PCIe"));
    ASSERT_EQ_INT(6, rewsr_pci_gen_from_speed_str("64.0 GT/s PCIe"));
    /* Pre-5.6 kernels drop the suffix. */
    ASSERT_EQ_INT(4, rewsr_pci_gen_from_speed_str("16.0 GT/s"));
    ASSERT_EQ_INT(0, rewsr_pci_gen_from_speed_str("Unknown speed"));
    ASSERT_EQ_INT(0, rewsr_pci_gen_from_speed_str("33 MHz"));
    ASSERT_EQ_INT(0, rewsr_pci_gen_from_speed_str(""));
    ASSERT_EQ_INT(0, rewsr_pci_gen_from_speed_str(NULL));
}

static int write_pci_device(const char *root, const char *addr,
                            const char *vendor, const char *device,
                            const char *class_id, const char *speed,
                            const char *width, const char *numa) {
    char rel[256];
    int rc = 0;
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/vendor", addr);
    rc |= rewsr_tutil_write_file(root, rel, vendor);
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/device", addr);
    rc |= rewsr_tutil_write_file(root, rel, device);
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/class", addr);
    rc |= rewsr_tutil_write_file(root, rel, class_id);
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/revision", addr);
    rc |= rewsr_tutil_write_file(root, rel, "0x02\n");
    if (speed != NULL) {
        snprintf(rel, sizeof(rel),
                 "sys/bus/pci/devices/%s/current_link_speed", addr);
        rc |= rewsr_tutil_write_file(root, rel, speed);
        snprintf(rel, sizeof(rel),
                 "sys/bus/pci/devices/%s/current_link_width", addr);
        rc |= rewsr_tutil_write_file(root, rel, width);
    }
    if (numa != NULL) {
        snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/numa_node",
                 addr);
        rc |= rewsr_tutil_write_file(root, rel, numa);
    }
    return rc;
}

static void test_scan_fixture_tree(void) {
    char root[256];
    ASSERT_EQ_INT(0, rewsr_tutil_mkroot(root, sizeof(root)));

    /* ConnectX-5 at gen4 x16 on node 1. */
    ASSERT_EQ_INT(0, write_pci_device(root, "0000:41:00.0", "0x15b3\n",
                                      "0x1017\n", "0x020000\n",
                                      "16.0 GT/s PCIe\n", "16\n", "1\n"));
    /* NVMe drive at gen3 x4, no numa file (reads as -1). */
    ASSERT_EQ_INT(0, write_pci_device(root, "0000:01:00.0", "0x144d\n",
                                      "0xa808\n", "0x010802\n",
                                      "8.0 GT/s PCIe\n", "4\n", NULL));
    /* Host bridge: no link files at all. */
    ASSERT_EQ_INT(0, write_pci_device(root, "0000:00:00.0", "0x1022\n",
                                      "0x1480\n", "0x060000\n", NULL,
                                      NULL, "0\n"));
    /* A stray non-device entry must be ignored. */
    ASSERT_EQ_INT(0, rewsr_tutil_write_file(
                         root, "sys/bus/pci/devices/rescan", "0\n"));

    /* Bind a driver to the NIC via the symlink convention. */
    {
        char link[512];
        snprintf(link, sizeof(link),
                 "%s/sys/bus/pci/devices/0000:41:00.0/driver", root);
        ASSERT_EQ_INT(0, symlink("../../../bus/pci/drivers/mlx5_core",
                                 link));
    }

    struct rewsr_pci_dev devs[8];
    size_t found = 0;
    ASSERT_EQ_INT(0, rewsr_pci_scan_root(root, devs, 8, &found));
    ASSERT_EQ_U64(3, found);

    /* Sorted by address. */
    ASSERT_EQ_STR("0000:00:00.0", devs[0].addr);
    ASSERT_EQ_STR("0000:01:00.0", devs[1].addr);
    ASSERT_EQ_STR("0000:41:00.0", devs[2].addr);

    ASSERT_EQ_U64(0x1022, devs[0].vendor);
    ASSERT_EQ_U64(0x060000, devs[0].class_id);
    ASSERT_EQ_INT(0, devs[0].link_gen);
    ASSERT_EQ_INT(-1, devs[0].link_width);
    ASSERT_EQ_INT(0, devs[0].numa_node);
    ASSERT_EQ_STR("", devs[0].driver);

    ASSERT_EQ_U64(0x144D, devs[1].vendor);
    ASSERT_EQ_INT(3, devs[1].link_gen);
    ASSERT_EQ_INT(4, devs[1].link_width);
    ASSERT_EQ_INT(-1, devs[1].numa_node);

    ASSERT_EQ_U64(0x15B3, devs[2].vendor);
    ASSERT_EQ_U64(0x1017, devs[2].device);
    ASSERT_EQ_INT(4, devs[2].link_gen);
    ASSERT_EQ_INT(16, devs[2].link_width);
    ASSERT_EQ_INT(1, devs[2].numa_node);
    ASSERT_EQ_INT(2, devs[2].revision);
    ASSERT_EQ_STR("16.0 GT/s PCIe", devs[2].link_speed);
    ASSERT_EQ_STR("mlx5_core", devs[2].driver);
    ASSERT_EQ_STR("Ethernet controller",
                  rewsr_pci_class_name(devs[2].class_id));

    rewsr_tutil_rmtree(root);
}

static void test_scan_missing_tree(void) {
    size_t found = 5;
    ASSERT_EQ_INT(-ENOENT,
                  rewsr_pci_scan_root("/nonexistent-rewsr", NULL, 0,
                                      &found));
    ASSERT_EQ_U64(0, found);
}

static void test_live_scan_platform_behavior(void) {
    struct rewsr_pci_dev devs[4];
    size_t found = 0;
    int rc = rewsr_pci_scan(devs, 4, &found);
#if defined(__linux__)
    /* On Linux this must reach the real sysfs; any box running the
     * suite has at least a host bridge. */
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(found > 0);
#else
    ASSERT_EQ_INT(-ENOTSUP, rc);
#endif
}

REWSR_TEST_MAIN(
    RUN_TEST(test_modalias_i350);
    RUN_TEST(test_modalias_connectx5_with_newline);
    RUN_TEST(test_modalias_nvme_lowercase);
    RUN_TEST(test_modalias_malformed);
    RUN_TEST(test_class_names);
    RUN_TEST(test_vendor_names);
    RUN_TEST(test_link_speed_mapping);
    RUN_TEST(test_scan_fixture_tree);
    RUN_TEST(test_scan_missing_tree);
    RUN_TEST(test_live_scan_platform_behavior);
)
