/*
 * rewsr_sysfs.c, implementation of the rooted sysfs readers.
 *
 * Deliberately plain POSIX (open/read, opendir/readdir, stat) so the
 * same code runs against fixture trees in tests on any host. sysfs
 * attributes are tiny, a single read covers them; we still loop on
 * EINTR because the collector runs under a supervisor that signals.
 */
#include "rewsr_sysfs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int join_path(char *dst, size_t cap, const char *root,
                     const char *relpath) {
    int n = snprintf(dst, cap, "%s/%s", root ? root : "", relpath);
    if (n < 0 || (size_t)n >= cap) {
        return -ENAMETOOLONG;
    }
    return 0;
}

int rewsr_read_first_line(const char *root, const char *relpath,
                          char *buf, size_t cap) {
    if (buf == NULL || cap == 0) {
        return -EINVAL;
    }
    buf[0] = '\0';

    char path[1024];
    int rc = join_path(path, sizeof(path), root, relpath);
    if (rc != 0) {
        return rc;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -errno;
    }

    size_t used = 0;
    for (;;) {
        if (used + 1 >= cap) {
            /* No newline yet and no room left: the line does not fit.
             * Truncating silently would corrupt facts like a long
             * device address, so refuse loudly. */
            close(fd);
            return -EMSGSIZE;
        }
        ssize_t r = read(fd, buf + used, cap - 1 - used);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            int saved = errno;
            close(fd);
            return -saved;
        }
        if (r == 0) {
            break;
        }
        used += (size_t)r;
        char *nl = memchr(buf, '\n', used);
        if (nl != NULL) {
            used = (size_t)(nl - buf);
            break;
        }
    }
    close(fd);
    buf[used] = '\0';
    return 0;
}

int rewsr_sysfs_read_u64(const char *root, const char *relpath,
                         uint64_t *out) {
    char line[128];
    int rc = rewsr_read_first_line(root, relpath, line, sizeof(line));
    if (rc != 0) {
        return rc;
    }
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0' || *p == '-') {
        /* Empty attribute, or negative where unsigned was expected;
         * the caller wanted a u64 and did not get one. */
        return -EINVAL;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 0);
    if (errno != 0) {
        return -errno;
    }
    if (end == p) {
        return -EINVAL;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return -EINVAL;
    }
    *out = (uint64_t)v;
    return 0;
}

int rewsr_read_int(const char *root, const char *relpath, long long *out) {
    char line[128];
    int rc = rewsr_read_first_line(root, relpath, line, sizeof(line));
    if (rc != 0) {
        return rc;
    }
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return -EINVAL;
    }
    errno = 0;
    char *end = NULL;
    long long v = strtoll(p, &end, 0);
    if (errno != 0) {
        return -errno;
    }
    if (end == p) {
        return -EINVAL;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return -EINVAL;
    }
    *out = v;
    return 0;
}

/*
 * Parse one unsigned decimal number out of a cpulist. Returns the
 * value via *out and advances *pp past the digits, -EINVAL when the
 * next character is not a digit.
 */
static int cpulist_number(const char **pp, uint32_t *out) {
    const char *p = *pp;
    if (!isdigit((unsigned char)*p)) {
        return -EINVAL;
    }
    uint64_t v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFull) {
            return -ERANGE;
        }
        p++;
    }
    *pp = p;
    *out = (uint32_t)v;
    return 0;
}

int rewsr_parse_cpulist(const char *s, uint64_t *bitmap, size_t nwords,
                        uint32_t *count) {
    if (s == NULL || bitmap == NULL || nwords == 0) {
        return -EINVAL;
    }
    memset(bitmap, 0, nwords * sizeof(uint64_t));

    const char *p = s;
    /* Kernel output ends with one newline; tolerate surrounding
     * whitespace so callers can feed raw file contents. */
    while (*p == ' ' || *p == '\t' || *p == '\n') {
        p++;
    }
    if (*p == '\0') {
        if (count != NULL) {
            *count = 0;
        }
        return 0;
    }

    for (;;) {
        uint32_t lo, hi;
        int rc = cpulist_number(&p, &lo);
        if (rc != 0) {
            return rc;
        }
        hi = lo;
        if (*p == '-') {
            p++;
            rc = cpulist_number(&p, &hi);
            if (rc != 0) {
                return rc;
            }
            if (hi < lo) {
                return -EINVAL;
            }
        }
        if ((size_t)hi / 64 >= nwords) {
            return -ERANGE;
        }
        for (uint32_t cpu = lo; cpu <= hi; cpu++) {
            bitmap[cpu / 64] |= 1ull << (cpu % 64);
            if (cpu == UINT32_MAX) {
                break; /* guard uint32 wrap on a hostile hi */
            }
        }

        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (*p != ',') {
            return -EINVAL;
        }
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            /* Trailing comma with nothing after it. */
            return -EINVAL;
        }
    }

    if (count != NULL) {
        uint32_t n = 0;
        for (size_t w = 0; w < nwords; w++) {
            uint64_t v = bitmap[w];
            while (v) {
                v &= v - 1;
                n++;
            }
        }
        *count = n;
    }
    return 0;
}

int rewsr_cpumask_test(const uint64_t *bitmap, size_t nwords, uint32_t cpu) {
    if (bitmap == NULL || (size_t)cpu / 64 >= nwords) {
        return 0;
    }
    return (bitmap[cpu / 64] >> (cpu % 64)) & 1 ? 1 : 0;
}

static void netdev_read_attrs(const char *root, const char *ifname,
                              struct rewsr_netdev *nd) {
    char rel[256];
    memset(nd, 0, sizeof(*nd));
    snprintf(nd->name, sizeof(nd->name), "%s", ifname);

    struct {
        const char *attr;
        long long *dst;
    } nums[] = {
        { "mtu", &nd->mtu },
        { "ifindex", &nd->ifindex },
        { "speed", &nd->speed_mbps },
        { "carrier", &nd->carrier },
        { "type", &nd->arphrd_type },
    };
    for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
        *nums[i].dst = -1;
        snprintf(rel, sizeof(rel), "sys/class/net/%s/%s", ifname,
                 nums[i].attr);
        long long v;
        if (rewsr_read_int(root, rel, &v) == 0) {
            *nums[i].dst = v;
        }
    }

    snprintf(rel, sizeof(rel), "sys/class/net/%s/operstate", ifname);
    rewsr_read_first_line(root, rel, nd->operstate, sizeof(nd->operstate));
    snprintf(rel, sizeof(rel), "sys/class/net/%s/address", ifname);
    rewsr_read_first_line(root, rel, nd->address, sizeof(nd->address));
    snprintf(rel, sizeof(rel), "sys/class/net/%s/duplex", ifname);
    rewsr_read_first_line(root, rel, nd->duplex, sizeof(nd->duplex));
}

static int netdev_name_cmp(const void *a, const void *b) {
    const struct rewsr_netdev *na = a;
    const struct rewsr_netdev *nb = b;
    return strcmp(na->name, nb->name);
}

int rewsr_sysfs_scan_net(const char *root, struct rewsr_netdev *out,
                         size_t cap, size_t *found) {
    if (found != NULL) {
        *found = 0;
    }
    char dirpath[1024];
    int rc = join_path(dirpath, sizeof(dirpath), root, "sys/class/net");
    if (rc != 0) {
        return rc;
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
        /* Entries like bonding_masters are plain files; interfaces
         * are directories (symlinks to them on a live sysfs, which
         * stat follows). */
        char child[1200];
        int n = snprintf(child, sizeof(child), "%s/%s", dirpath,
                         ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        total++;
        if (out != NULL && filled < cap) {
            netdev_read_attrs(root, ent->d_name, &out[filled]);
            filled++;
        }
    }
    closedir(d);

    if (out != NULL && filled > 1) {
        qsort(out, filled, sizeof(out[0]), netdev_name_cmp);
    }
    if (found != NULL) {
        *found = total;
    }
    return 0;
}
