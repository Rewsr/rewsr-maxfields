/* block.h: block device inventory from the sysfs block tree.
 *
 * The readers take a sysfs root prefix: "/sys" on a live host, a fixture
 * directory under test, so they compile and run anywhere. Only
 * rewsr_block_collect() hardcodes the live path and it returns -ENOTSUP
 * off Linux.
 *
 * Layout read per device, relative to <root>/block/<name>:
 *   size                       whole-device length in 512-byte sectors,
 *                              regardless of logical block size
 *   queue/rotational           1 spinning, 0 solid state
 *   queue/nr_requests          queue depth
 *   queue/scheduler            all schedulers, active one in brackets,
 *                              e.g. "[mq-deadline] none"
 *   queue/logical_block_size   bytes
 *   device/model               space-padded on SCSI, plain on NVMe
 *   device/vendor              SCSI only; absent for NVMe
 */
#ifndef REWSR_CSTORE_BLOCK_H
#define REWSR_CSTORE_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#define REWSR_BLOCK_NAME_MAX  64
#define REWSR_BLOCK_MODEL_MAX 128
#define REWSR_BLOCK_SCHED_MAX 32

/* Scan reads at most this many entries out of <root>/block. */
#define REWSR_BLOCK_SCAN_MAX 128

typedef enum rewsr_block_kind {
    REWSR_BLOCK_KIND_UNKNOWN = 0,
    REWSR_BLOCK_KIND_NVME,   /* nvme0n1 */
    REWSR_BLOCK_KIND_SCSI,   /* sd*: SATA, SAS, USB mass storage */
    REWSR_BLOCK_KIND_VIRTIO, /* vd* */
    REWSR_BLOCK_KIND_XEN,    /* xvd* */
    REWSR_BLOCK_KIND_MMC,    /* mmcblk* */
    REWSR_BLOCK_KIND_CDROM,  /* sr* */
    REWSR_BLOCK_KIND_LOOP,
    REWSR_BLOCK_KIND_RAM,
    REWSR_BLOCK_KIND_ZRAM,
    REWSR_BLOCK_KIND_DM,     /* dm-*: device mapper */
    REWSR_BLOCK_KIND_MD      /* md*: software raid */
} rewsr_block_kind;

typedef struct rewsr_block_device {
    char name[REWSR_BLOCK_NAME_MAX];
    rewsr_block_kind kind;
    uint64_t size_sectors;   /* 512-byte units, straight from sysfs */
    uint64_t size_bytes;
    uint64_t size_gb;        /* decimal GB, rounded to nearest */
    int rotational;          /* 1 spinning, 0 solid state, -1 unknown */
    uint32_t nr_requests;    /* 0 when the attribute is absent */
    uint32_t logical_block_size;
    char scheduler[REWSR_BLOCK_SCHED_MAX]; /* active one, brackets stripped */
    char model[REWSR_BLOCK_MODEL_MAX];
    char vendor[REWSR_BLOCK_MODEL_MAX];
} rewsr_block_device;

/* Pure helpers, no filesystem access. */

/* 512-byte sectors to bytes; saturates at UINT64_MAX instead of wrapping. */
uint64_t rewsr_block_sectors_to_bytes(uint64_t sectors);

/* 512-byte sectors to decimal gigabytes (1 GB = 1e9 bytes), rounded to
 * nearest. Matches how drives are marketed, so a 1953525168-sector disk
 * reports 1000 GB. */
uint64_t rewsr_block_sectors_to_gb(uint64_t sectors);

/* Extract the active scheduler from a queue/scheduler line. The active
 * entry is bracketed: "[mq-deadline] none" yields "mq-deadline". A line
 * with a single bare token (older single-scheduler kernels) yields that
 * token. Multiple tokens without brackets are ambiguous: -EINVAL. */
int rewsr_block_active_scheduler(const char *line, char *out, size_t cap);

rewsr_block_kind rewsr_block_classify(const char *name);
const char *rewsr_block_kind_str(rewsr_block_kind kind);

/* Nonzero for kinds that never map to hardware: loop, ram, zram, dm, md. */
int rewsr_block_is_virtual(rewsr_block_kind kind);

/* Read one device. Missing individual attributes are tolerated and leave
 * their defaults; a missing device directory is -ENOENT. */
int rewsr_block_read_device(const char *sysfs_root, const char *name,
                            rewsr_block_device *out);

/* Scan <root>/block in sorted name order. Fills at most cap devices and
 * returns the number stored, or -errno if the tree cannot be listed.
 * Entries that vanish mid-scan are skipped. A full return equal to cap
 * may mean truncation. */
int rewsr_block_scan(const char *sysfs_root, rewsr_block_device *devs,
                     size_t cap);

/* Linux only: scan the live /sys. -ENOTSUP elsewhere. */
int rewsr_block_collect(rewsr_block_device *devs, size_t cap);

#endif
