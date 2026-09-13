#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "block.h"
#include "../util/util.h"

#include <errno.h>
#include <string.h>

uint64_t rewsr_block_sectors_to_bytes(uint64_t sectors)
{
    if (sectors > UINT64_MAX / 512)
        return UINT64_MAX;
    return sectors * 512;
}

uint64_t rewsr_block_sectors_to_gb(uint64_t sectors)
{
    uint64_t bytes = rewsr_block_sectors_to_bytes(sectors);
    uint64_t q = bytes / 1000000000ULL;
    uint64_t r = bytes % 1000000000ULL;

    return q + (r >= 500000000ULL ? 1 : 0);
}

int rewsr_block_active_scheduler(const char *line, char *out, size_t cap)
{
    const char *start;
    const char *end;
    const char *open;
    size_t n;

    if (line == NULL || out == NULL || cap == 0)
        return -EINVAL;

    open = strchr(line, '[');
    if (open != NULL) {
        start = open + 1;
        end = strchr(start, ']');
        if (end == NULL)
            return -EINVAL;
    } else {
        const char *rest;

        while (*line == ' ' || *line == '\t' || *line == '\n')
            line++;
        start = line;
        end = start;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n')
            end++;
        rest = end;
        while (*rest == ' ' || *rest == '\t' || *rest == '\n')
            rest++;
        /* a second bare token means we cannot tell which one is active */
        if (*rest != '\0')
            return -EINVAL;
    }

    n = (size_t)(end - start);
    if (n == 0)
        return -EINVAL;
    if (n >= cap)
        return -ENAMETOOLONG;
    memcpy(out, start, n);
    out[n] = '\0';
    return 0;
}

static int has_prefix(const char *s, const char *pre)
{
    return strncmp(s, pre, strlen(pre)) == 0;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

rewsr_block_kind rewsr_block_classify(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return REWSR_BLOCK_KIND_UNKNOWN;
    if (has_prefix(name, "nvme"))
        return REWSR_BLOCK_KIND_NVME;
    if (has_prefix(name, "mmcblk"))
        return REWSR_BLOCK_KIND_MMC;
    /* zram before ram: shared prefix */
    if (has_prefix(name, "zram"))
        return REWSR_BLOCK_KIND_ZRAM;
    if (has_prefix(name, "ram"))
        return REWSR_BLOCK_KIND_RAM;
    if (has_prefix(name, "loop"))
        return REWSR_BLOCK_KIND_LOOP;
    if (has_prefix(name, "xvd"))
        return REWSR_BLOCK_KIND_XEN;
    if (has_prefix(name, "dm-"))
        return REWSR_BLOCK_KIND_DM;
    if (has_prefix(name, "md") && is_digit(name[2]))
        return REWSR_BLOCK_KIND_MD;
    if (has_prefix(name, "sr") && is_digit(name[2]))
        return REWSR_BLOCK_KIND_CDROM;
    if (has_prefix(name, "sd") && name[2] >= 'a' && name[2] <= 'z')
        return REWSR_BLOCK_KIND_SCSI;
    if (has_prefix(name, "vd") && name[2] >= 'a' && name[2] <= 'z')
        return REWSR_BLOCK_KIND_VIRTIO;
    return REWSR_BLOCK_KIND_UNKNOWN;
}

const char *rewsr_block_kind_str(rewsr_block_kind kind)
{
    switch (kind) {
    case REWSR_BLOCK_KIND_NVME:
        return "nvme";
    case REWSR_BLOCK_KIND_SCSI:
        return "scsi";
    case REWSR_BLOCK_KIND_VIRTIO:
        return "virtio";
    case REWSR_BLOCK_KIND_XEN:
        return "xen";
    case REWSR_BLOCK_KIND_MMC:
        return "mmc";
    case REWSR_BLOCK_KIND_CDROM:
        return "cdrom";
    case REWSR_BLOCK_KIND_LOOP:
        return "loop";
    case REWSR_BLOCK_KIND_RAM:
        return "ram";
    case REWSR_BLOCK_KIND_ZRAM:
        return "zram";
    case REWSR_BLOCK_KIND_DM:
        return "dm";
    case REWSR_BLOCK_KIND_MD:
        return "md";
    case REWSR_BLOCK_KIND_UNKNOWN:
        break;
    }
    return "unknown";
}

int rewsr_block_is_virtual(rewsr_block_kind kind)
{
    switch (kind) {
    case REWSR_BLOCK_KIND_LOOP:
    case REWSR_BLOCK_KIND_RAM:
    case REWSR_BLOCK_KIND_ZRAM:
    case REWSR_BLOCK_KIND_DM:
    case REWSR_BLOCK_KIND_MD:
        return 1;
    default:
        return 0;
    }
}

int rewsr_block_read_device(const char *sysfs_root, const char *name,
                            rewsr_block_device *out)
{
    char base[REWSR_UTIL_PATH_MAX];
    char val[256];
    uint64_t u64v;
    uint32_t u32v;
    int rc;

    if (sysfs_root == NULL || name == NULL || out == NULL)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    rc = rewsr_strlcpy(out->name, sizeof(out->name), name);
    if (rc != 0)
        return rc;
    out->kind = rewsr_block_classify(name);
    out->rotational = -1;

    rc = rewsr_path_join(base, sizeof(base), sysfs_root, "block");
    if (rc != 0)
        return rc;
    rc = rewsr_path_join(base, sizeof(base), base, name);
    if (rc != 0)
        return rc;
    if (!rewsr_dir_exists(base, "."))
        return -ENOENT;

    if (rewsr_read_attr_u64(base, "size", &u64v) == 0) {
        out->size_sectors = u64v;
        out->size_bytes = rewsr_block_sectors_to_bytes(u64v);
        out->size_gb = rewsr_block_sectors_to_gb(u64v);
    }
    if (rewsr_read_attr_u32(base, "queue/rotational", &u32v) == 0)
        out->rotational = (u32v != 0);
    if (rewsr_read_attr_u32(base, "queue/nr_requests", &u32v) == 0)
        out->nr_requests = u32v;
    if (rewsr_read_attr_u32(base, "queue/logical_block_size", &u32v) == 0)
        out->logical_block_size = u32v;

    if (rewsr_read_attr(base, "queue/scheduler", val, sizeof(val)) >= 0)
        (void)rewsr_block_active_scheduler(val, out->scheduler,
                                           sizeof(out->scheduler));

    if (rewsr_read_attr(base, "device/model", val, sizeof(val)) >= 0)
        (void)rewsr_strlcpy(out->model, sizeof(out->model), val);
    if (rewsr_read_attr(base, "device/vendor", val, sizeof(val)) >= 0)
        (void)rewsr_strlcpy(out->vendor, sizeof(out->vendor), val);

    return 0;
}

int rewsr_block_scan(const char *sysfs_root, rewsr_block_device *devs,
                     size_t cap)
{
    char dirpath[REWSR_UTIL_PATH_MAX];
    char names[REWSR_BLOCK_SCAN_MAX][REWSR_UTIL_NAME_MAX];
    int total;
    int i;
    int n;
    size_t stored = 0;

    if (sysfs_root == NULL || (devs == NULL && cap > 0))
        return -EINVAL;
    if (rewsr_path_join(dirpath, sizeof(dirpath), sysfs_root, "block") != 0)
        return -ENAMETOOLONG;

    total = rewsr_list_dir(dirpath, names, REWSR_BLOCK_SCAN_MAX);
    if (total < 0)
        return total;
    n = total > REWSR_BLOCK_SCAN_MAX ? REWSR_BLOCK_SCAN_MAX : total;

    for (i = 0; i < n && stored < cap; i++) {
        if (rewsr_block_read_device(sysfs_root, names[i], &devs[stored]) == 0)
            stored++;
    }
    return (int)stored;
}

int rewsr_block_collect(rewsr_block_device *devs, size_t cap)
{
#if defined(__linux__)
    return rewsr_block_scan("/sys", devs, cap);
#else
    (void)devs;
    (void)cap;
    return -ENOTSUP;
#endif
}
