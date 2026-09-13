/*
 * rewsr_pci.h, PCI inventory without shelling out to lspci.
 *
 * Three layers:
 *   1. Pure string/number helpers (modalias parsing, class and vendor
 *      names, link-speed mapping). No IO at all.
 *   2. rewsr_pci_scan_root(), a rooted scan of
 *      <root>/sys/bus/pci/devices that only does POSIX IO, so tests
 *      drive it against a fixture tree on any host.
 *   3. rewsr_pci_scan(), the Linux entry point, equivalent to scanning
 *      root "" and -ENOTSUP elsewhere (rewsr_pci_linux.c).
 *
 * The modalias format is produced by drivers/pci/pci-sysfs.c
 * (modalias_show) as:
 *   pci:v%08Xd%08Xsv%08Xsd%08Xbc%02Xsc%02Xi%02X
 * i.e. "pci:v000015B3d00001017sv000015B3sd00000020bc02sc00i00" for a
 * ConnectX-5. Vendor/device/subvendor/subdevice are 8 uppercase hex
 * digits (16 significant bits today), base class/subclass/prog-if two
 * digits each.
 */
#ifndef REWSR_PCI_H
#define REWSR_PCI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rewsr_pci_id {
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;
    uint32_t subdevice;
    uint8_t base_class;
    uint8_t subclass;
    uint8_t prog_if;
};

/*
 * Parse a sysfs modalias string. Tolerates a trailing newline (raw
 * file contents feed straight in) and lowercase hex, rejects anything
 * that deviates from the fixed tag order or digit counts with
 * -EINVAL.
 */
int rewsr_pci_parse_modalias(const char *s, struct rewsr_pci_id *out);

/*
 * Class name for a 24-bit class dword as sysfs reports it
 * (base << 16 | subclass << 8 | prog_if, e.g. 0x020000 Ethernet,
 * 0x010802 NVMe). Falls back from (base, subclass) to base-only,
 * returns NULL for codes with no name so the caller renders the hex
 * instead of a lie.
 */
const char *rewsr_pci_class_name(uint32_t class_id);

/* Vendor names for ids that actually appear in this fleet, NULL
 * otherwise. Sourced from the PCI SIG registry. */
const char *rewsr_pci_vendor_name(uint16_t vendor);

/*
 * Map the sysfs current_link_speed string to a PCIe generation:
 * "2.5 GT/s PCIe" -> 1 ... "64.0 GT/s PCIe" -> 6. Kernels before 5.6
 * omit the trailing " PCIe" and that also parses. 0 = unrecognized
 * (including the literal "Unknown speed" the kernel emits for ports
 * in a weird state).
 */
int rewsr_pci_gen_from_speed_str(const char *s);

struct rewsr_pci_dev {
    char addr[16]; /* domain:bus:dev.fn, "0000:41:00.0" */
    uint32_t vendor;
    uint32_t device;
    uint32_t class_id;
    uint8_t revision;
    long long numa_node;   /* -1 when unassigned */
    int link_gen;          /* 0 unknown */
    long long link_width;  /* -1 unknown */
    char link_speed[32];   /* raw sysfs string */
    char driver[64];       /* bound driver name, empty if none */
};

/*
 * Rooted scan of <root>/sys/bus/pci/devices. Fills at most cap
 * entries sorted by address, sets *found to the total present (may
 * exceed cap). Devices missing optional attributes (numa_node,
 * link_speed on legacy buses, no bound driver) still produce entries
 * with the documented "unknown" values. Returns 0 or negative errno
 * when the devices directory itself cannot be opened.
 */
int rewsr_pci_scan_root(const char *root, struct rewsr_pci_dev *out,
                        size_t cap, size_t *found);

/* Live-system scan, Linux only (-ENOTSUP elsewhere). */
int rewsr_pci_scan(struct rewsr_pci_dev *out, size_t cap, size_t *found);

#ifdef __cplusplus
}
#endif

#endif /* REWSR_PCI_H */
