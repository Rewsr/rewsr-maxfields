/*
 * rewsr_pci.c, portable PCI helpers and the rooted device scan.
 *
 * The class table is transcribed from the PCI Code and ID Assignment
 * Specification (the same source as linux/pci_ids.h); entries are
 * limited to hardware that can plausibly show up in a server, which
 * is still most of the table.
 */
#include "rewsr_pci.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../sysfs/rewsr_sysfs.h"

/* Parse exactly n hex digits, advancing *pp. -EINVAL on any
 * non-hex-digit inside the run. */
static int hex_fixed(const char **pp, int n, uint32_t *out) {
    const char *p = *pp;
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        char c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return -EINVAL;
        }
        v = (v << 4) | d;
    }
    *pp = p + n;
    *out = v;
    return 0;
}

static int expect(const char **pp, const char *tag) {
    size_t n = strlen(tag);
    if (strncmp(*pp, tag, n) != 0) {
        return -EINVAL;
    }
    *pp += n;
    return 0;
}

int rewsr_pci_parse_modalias(const char *s, struct rewsr_pci_id *out) {
    if (s == NULL || out == NULL) {
        return -EINVAL;
    }
    memset(out, 0, sizeof(*out));
    const char *p = s;
    uint32_t v;
    int rc;

    if ((rc = expect(&p, "pci:v")) != 0) return rc;
    if ((rc = hex_fixed(&p, 8, &out->vendor)) != 0) return rc;
    if ((rc = expect(&p, "d")) != 0) return rc;
    if ((rc = hex_fixed(&p, 8, &out->device)) != 0) return rc;
    if ((rc = expect(&p, "sv")) != 0) return rc;
    if ((rc = hex_fixed(&p, 8, &out->subvendor)) != 0) return rc;
    if ((rc = expect(&p, "sd")) != 0) return rc;
    if ((rc = hex_fixed(&p, 8, &out->subdevice)) != 0) return rc;
    if ((rc = expect(&p, "bc")) != 0) return rc;
    if ((rc = hex_fixed(&p, 2, &v)) != 0) return rc;
    out->base_class = (uint8_t)v;
    if ((rc = expect(&p, "sc")) != 0) return rc;
    if ((rc = hex_fixed(&p, 2, &v)) != 0) return rc;
    out->subclass = (uint8_t)v;
    if ((rc = expect(&p, "i")) != 0) return rc;
    if ((rc = hex_fixed(&p, 2, &v)) != 0) return rc;
    out->prog_if = (uint8_t)v;

    /* Only whitespace may follow (sysfs appends one newline). */
    while (*p == '\n' || *p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '\0') {
        return -EINVAL;
    }
    return 0;
}

struct pci_class_entry {
    uint8_t base;
    int sub; /* -1 = base-class fallback row */
    const char *name;
};

static const struct pci_class_entry k_pci_classes[] = {
    { 0x00, -1,   "Unclassified device" },
    { 0x00, 0x00, "Non-VGA unclassified device" },
    { 0x00, 0x01, "VGA compatible unclassified device" },

    { 0x01, -1,   "Mass storage controller" },
    { 0x01, 0x00, "SCSI storage controller" },
    { 0x01, 0x01, "IDE interface" },
    { 0x01, 0x02, "Floppy disk controller" },
    { 0x01, 0x04, "RAID bus controller" },
    { 0x01, 0x05, "ATA controller" },
    { 0x01, 0x06, "SATA controller" },
    { 0x01, 0x07, "Serial Attached SCSI controller" },
    { 0x01, 0x08, "Non-Volatile memory controller" },
    { 0x01, 0x80, "Mass storage controller" },

    { 0x02, -1,   "Network controller" },
    { 0x02, 0x00, "Ethernet controller" },
    { 0x02, 0x01, "Token ring network controller" },
    { 0x02, 0x02, "FDDI network controller" },
    { 0x02, 0x04, "ISDN controller" },
    { 0x02, 0x07, "Infiniband controller" },
    { 0x02, 0x08, "Fabric controller" },
    { 0x02, 0x80, "Network controller" },

    { 0x03, -1,   "Display controller" },
    { 0x03, 0x00, "VGA compatible controller" },
    { 0x03, 0x01, "XGA compatible controller" },
    { 0x03, 0x02, "3D controller" },
    { 0x03, 0x80, "Display controller" },

    { 0x04, -1,   "Multimedia controller" },
    { 0x04, 0x00, "Multimedia video controller" },
    { 0x04, 0x01, "Multimedia audio controller" },
    { 0x04, 0x03, "Audio device" },

    { 0x05, -1,   "Memory controller" },
    { 0x05, 0x00, "RAM memory" },
    { 0x05, 0x80, "Memory controller" },

    { 0x06, -1,   "Bridge" },
    { 0x06, 0x00, "Host bridge" },
    { 0x06, 0x01, "ISA bridge" },
    { 0x06, 0x04, "PCI bridge" },
    { 0x06, 0x09, "PCI-to-PCI bridge (semi-transparent)" },
    { 0x06, 0x80, "Bridge" },

    { 0x07, -1,   "Communication controller" },
    { 0x07, 0x00, "Serial controller" },
    { 0x07, 0x02, "Multiport serial controller" },
    { 0x07, 0x80, "Communication controller" },

    { 0x08, -1,   "Generic system peripheral" },
    { 0x08, 0x00, "PIC" },
    { 0x08, 0x01, "DMA controller" },
    { 0x08, 0x02, "Timer" },
    { 0x08, 0x03, "RTC" },
    { 0x08, 0x05, "SD Host controller" },
    { 0x08, 0x06, "IOMMU" },
    { 0x08, 0x80, "System peripheral" },

    { 0x09, -1,   "Input device controller" },
    { 0x0A, -1,   "Docking station" },

    { 0x0B, -1,   "Processor" },
    { 0x0B, 0x40, "Co-processor" },

    { 0x0C, -1,   "Serial bus controller" },
    { 0x0C, 0x00, "FireWire (IEEE 1394)" },
    { 0x0C, 0x03, "USB controller" },
    { 0x0C, 0x04, "Fibre Channel" },
    { 0x0C, 0x05, "SMBus" },
    { 0x0C, 0x06, "InfiniBand" },
    { 0x0C, 0x80, "Serial bus controller" },

    { 0x0D, -1,   "Wireless controller" },
    { 0x0D, 0x11, "Bluetooth" },
    { 0x0D, 0x80, "Wireless controller" },

    { 0x0E, -1,   "Intelligent controller" },
    { 0x0F, -1,   "Satellite communications controller" },

    { 0x10, -1,   "Encryption controller" },
    { 0x10, 0x00, "Network and computing encryption device" },
    { 0x10, 0x80, "Encryption controller" },

    { 0x11, -1,   "Signal processing controller" },
    { 0x12, -1,   "Processing accelerator" },
    { 0x12, 0x00, "Processing accelerator" },
    { 0x13, -1,   "Non-Essential Instrumentation" },
    { 0x40, -1,   "Coprocessor" },
};

const char *rewsr_pci_class_name(uint32_t class_id) {
    uint8_t base = (uint8_t)((class_id >> 16) & 0xFF);
    uint8_t sub = (uint8_t)((class_id >> 8) & 0xFF);
    const char *fallback = NULL;
    for (size_t i = 0; i < sizeof(k_pci_classes) / sizeof(k_pci_classes[0]);
         i++) {
        const struct pci_class_entry *e = &k_pci_classes[i];
        if (e->base != base) {
            continue;
        }
        if (e->sub == (int)sub) {
            return e->name;
        }
        if (e->sub == -1) {
            fallback = e->name;
        }
    }
    return fallback;
}

const char *rewsr_pci_vendor_name(uint16_t vendor) {
    /* IDs from the PCI SIG registry; the short list a bare-metal
     * fleet actually reports. */
    switch (vendor) {
    case 0x1000: return "Broadcom / LSI";
    case 0x1002: return "AMD/ATI";
    case 0x1014: return "IBM";
    case 0x1022: return "AMD";
    case 0x102B: return "Matrox";
    case 0x1077: return "QLogic";
    case 0x10B5: return "PLX Technology";
    case 0x10DE: return "NVIDIA";
    case 0x10EE: return "Xilinx";
    case 0x1137: return "Cisco";
    case 0x11F8: return "PMC-Sierra";
    case 0x1344: return "Micron Technology";
    case 0x144D: return "Samsung Electronics";
    case 0x14E4: return "Broadcom";
    case 0x15B3: return "Mellanox Technologies";
    case 0x1657: return "Brocade";
    case 0x168C: return "Qualcomm Atheros";
    case 0x17CB: return "Qualcomm";
    case 0x1912: return "Renesas";
    case 0x19A2: return "Emulex";
    case 0x19E5: return "Huawei";
    case 0x1AF4: return "Red Hat (virtio)";
    case 0x1B36: return "Red Hat (QEMU)";
    case 0x1B4B: return "Marvell";
    case 0x1C58: return "HGST";
    case 0x1C5C: return "SK hynix";
    case 0x1CC1: return "ADATA";
    case 0x1D0F: return "Amazon Annapurna Labs";
    case 0x1D6A: return "Aquantia";
    case 0x1DED: return "Alibaba (Pensando)";
    case 0x1E0F: return "KIOXIA";
    case 0x8086: return "Intel";
    case 0x9005: return "Adaptec";
    default:     return NULL;
    }
}

int rewsr_pci_gen_from_speed_str(const char *s) {
    if (s == NULL) {
        return 0;
    }
    /* Kernel strings: "2.5 GT/s PCIe", "5.0 GT/s PCIe", "8.0 GT/s
     * PCIe", "16.0 GT/s PCIe", "32.0 GT/s PCIe", "64.0 GT/s PCIe";
     * pre-5.6 kernels drop the " PCIe" suffix. Match on the numeric
     * prefix and require the GT/s unit. */
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s) {
        return 0;
    }
    while (*end == ' ') {
        end++;
    }
    if (strncmp(end, "GT/s", 4) != 0) {
        return 0;
    }
    if (v == 2.5) return 1;
    if (v == 5.0) return 2;
    if (v == 8.0) return 3;
    if (v == 16.0) return 4;
    if (v == 32.0) return 5;
    if (v == 64.0) return 6;
    return 0;
}

static int pci_dev_cmp(const void *a, const void *b) {
    const struct rewsr_pci_dev *da = a;
    const struct rewsr_pci_dev *db = b;
    return strcmp(da->addr, db->addr);
}

static void pci_read_device(const char *root, const char *addr,
                            struct rewsr_pci_dev *dev) {
    char rel[256];
    memset(dev, 0, sizeof(*dev));
    snprintf(dev->addr, sizeof(dev->addr), "%s", addr);
    dev->numa_node = -1;
    dev->link_width = -1;

    uint64_t v;
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/vendor", addr);
    if (rewsr_sysfs_read_u64(root, rel, &v) == 0) {
        dev->vendor = (uint32_t)v;
    }
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/device", addr);
    if (rewsr_sysfs_read_u64(root, rel, &v) == 0) {
        dev->device = (uint32_t)v;
    }
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/class", addr);
    if (rewsr_sysfs_read_u64(root, rel, &v) == 0) {
        dev->class_id = (uint32_t)v;
    }
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/revision", addr);
    if (rewsr_sysfs_read_u64(root, rel, &v) == 0) {
        dev->revision = (uint8_t)v;
    }

    long long ll;
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/numa_node", addr);
    if (rewsr_read_int(root, rel, &ll) == 0) {
        dev->numa_node = ll;
    }
    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/current_link_width",
             addr);
    if (rewsr_read_int(root, rel, &ll) == 0) {
        dev->link_width = ll;
    }

    snprintf(rel, sizeof(rel), "sys/bus/pci/devices/%s/current_link_speed",
             addr);
    if (rewsr_read_first_line(root, rel, dev->link_speed,
                              sizeof(dev->link_speed)) == 0) {
        dev->link_gen = rewsr_pci_gen_from_speed_str(dev->link_speed);
    }

    /* The bound driver is a symlink named "driver"; its target
     * basename is the driver name ("mlx5_core", "nvme"). No driver
     * bound leaves the field empty. */
    char full[1024];
    int n = snprintf(full, sizeof(full),
                     "%s/sys/bus/pci/devices/%s/driver",
                     root ? root : "", addr);
    if (n > 0 && (size_t)n < sizeof(full)) {
        char target[512];
        ssize_t tn = readlink(full, target, sizeof(target) - 1);
        if (tn > 0) {
            target[tn] = '\0';
            const char *base = strrchr(target, '/');
            snprintf(dev->driver, sizeof(dev->driver), "%s",
                     base ? base + 1 : target);
        }
    }
}

int rewsr_pci_scan_root(const char *root, struct rewsr_pci_dev *out,
                        size_t cap, size_t *found) {
    if (found != NULL) {
        *found = 0;
    }
    char dirpath[1024];
    int n = snprintf(dirpath, sizeof(dirpath), "%s/sys/bus/pci/devices",
                     root ? root : "");
    if (n < 0 || (size_t)n >= sizeof(dirpath)) {
        return -ENAMETOOLONG;
    }

    DIR *d = opendir(dirpath);
    if (d == NULL) {
        return -errno;
    }

    size_t total = 0;
    size_t filled = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        /* Device entries are "dddd:bb:dd.f"; anything else in the
         * directory is not a device. */
        if (strchr(ent->d_name, ':') == NULL) {
            continue;
        }
        char child[1200];
        int cn = snprintf(child, sizeof(child), "%s/%s", dirpath,
                          ent->d_name);
        if (cn < 0 || (size_t)cn >= sizeof(child)) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        total++;
        if (out != NULL && filled < cap) {
            pci_read_device(root, ent->d_name, &out[filled]);
            filled++;
        }
    }
    closedir(d);

    if (out != NULL && filled > 1) {
        qsort(out, filled, sizeof(out[0]), pci_dev_cmp);
    }
    if (found != NULL) {
        *found = total;
    }
    return 0;
}
