/*
 * rewsr_sysfs.h, small fast readers for sysfs-shaped file trees.
 *
 * Every entry point takes a root prefix that is glued in front of the
 * relative path ("" targets the live filesystem, a fixture directory
 * targets a fake tree in tests). That one decision is why this whole
 * file is portable: the functions only do POSIX file IO, so the
 * parsers and the net scan run and get tested on macOS even though
 * their production input only exists on Linux.
 *
 * Conventions:
 *   relpath never starts with '/', the joined path is "<root>/<relpath>"
 *   returns 0 on success, negative errno on failure
 *   sysfs attributes are single short lines; buffers of 256 bytes are
 *   generous, overflow reports -EMSGSIZE instead of truncating silently
 */
#ifndef REWSR_SYSFS_H
#define REWSR_SYSFS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read the first line of root/relpath into buf, stripping the
 * trailing newline. buf is NUL-terminated on success. Empty files
 * yield an empty string and success (sysfs uses them as flags).
 */
int rewsr_read_first_line(const char *root, const char *relpath,
                          char *buf, size_t cap);

/*
 * Parse the first line as an unsigned 64-bit integer, strtoull base 0
 * so decimal and 0x-prefixed hex both work (PCI ids are "0x8086",
 * cpufreq values are decimal). Trailing whitespace is fine, any other
 * trailing junk is -EINVAL. Note base 0 also means a leading zero
 * reads as octal; sysfs does not emit octal-significant values.
 */
int rewsr_sysfs_read_u64(const char *root, const char *relpath,
                         uint64_t *out);

/*
 * Signed variant. Exists because a handful of attributes are
 * legitimately negative: net/<if>/speed is -1 while the link is down,
 * numa_node is -1 on single-node or unassigned devices.
 */
int rewsr_read_int(const char *root, const char *relpath, long long *out);

/*
 * Parse a kernel cpulist string ("0-3,8-11,16") into a bitmap.
 * bitmap is nwords u64 words, bit (cpu % 64) of word (cpu / 64);
 * words are zeroed first. count receives the number of distinct CPUs
 * set. The empty string (or bare newline) is a valid empty list, as
 * emitted for an offline mask. Errors:
 *   -EINVAL malformed token, reversed range, empty element
 *   -ERANGE cpu number does not fit in the provided bitmap
 */
int rewsr_parse_cpulist(const char *s, uint64_t *bitmap, size_t nwords,
                        uint32_t *count);

/* Bit test on a bitmap filled by rewsr_parse_cpulist. Out-of-range
 * CPUs read as 0. */
int rewsr_cpumask_test(const uint64_t *bitmap, size_t nwords, uint32_t cpu);

/*
 * One interface from a /sys/class/net scan. Numeric fields are -1
 * when the attribute is missing or unreadable (a bridge has no
 * "speed"; "carrier" reads EINVAL while the interface is down, which
 * also lands as -1 here). String fields are empty when missing.
 * address[] is sized for InfiniBand's 59-character hardware address,
 * not just a 17-character MAC.
 */
struct rewsr_netdev {
    char name[64];
    long long mtu;
    long long ifindex;
    long long speed_mbps;
    long long carrier;
    long long arphrd_type; /* uapi if_arp.h: 1 ARPHRD_ETHER, 772 LOOPBACK */
    char operstate[24];
    char address[64];
    char duplex[16];
};

/*
 * Scan root/sys/class/net. Fills at most cap entries sorted by name
 * (readdir order is filesystem-dependent and the facts pipeline diffs
 * runs, so determinism matters). *found gets the total number of
 * interfaces present, which can exceed cap; the caller detects
 * truncation by comparing. Non-directory entries are skipped, which
 * matters because /sys/class/net contains regular files when bonding
 * is loaded (bonding_masters).
 */
int rewsr_sysfs_scan_net(const char *root, struct rewsr_netdev *out,
                         size_t cap, size_t *found);

#ifdef __cplusplus
}
#endif

#endif /* REWSR_SYSFS_H */
