/*
 * rewsr_dmi_linux.c, the platform-bound half of the DMI probe.
 *
 * Reads the raw structure table the kernel exposes at
 * /sys/firmware/dmi/tables/DMI and hands it to the portable parser.
 * Kept separate from rewsr_dmi.c so the parser itself carries no
 * platform guards and stays testable on the macOS dev box.
 *
 * Reading this file needs root (mode 0400) on stock kernels, which is
 * fine: the facts collector already runs privileged for ethtool and
 * MSR access.
 */
#include "rewsr_dmi.h"

#include <errno.h>

#if defined(__linux__)

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Structure tables on current servers run 5-20KB; 4MB is far beyond
 * anything real and bounds a confused read (sysfs reports size 0 for
 * some binary attributes, so we cannot pre-stat).
 */
#define REWSR_DMI_MAX_TABLE (4u * 1024u * 1024u)

int rewsr_dmi_read_sysfs(const char *root, struct rewsr_dmi_info *out) {
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/sys/firmware/dmi/tables/DMI",
                     root ? root : "");
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -ENAMETOOLONG;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }

    size_t cap = 64 * 1024;
    size_t len = 0;
    uint8_t *buf = malloc(cap);
    if (buf == NULL) {
        close(fd);
        return -ENOMEM;
    }

    for (;;) {
        if (len == cap) {
            if (cap >= REWSR_DMI_MAX_TABLE) {
                free(buf);
                close(fd);
                return -EFBIG;
            }
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                close(fd);
                return -ENOMEM;
            }
            buf = nb;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            int saved = errno;
            free(buf);
            close(fd);
            return -saved;
        }
        if (r == 0) {
            break;
        }
        len += (size_t)r;
    }
    close(fd);

    int rc = rewsr_dmi_parse(buf, len, out);
    free(buf);
    return rc;
}

#else /* !__linux__ */

int rewsr_dmi_read_sysfs(const char *root, struct rewsr_dmi_info *out) {
    (void)root;
    (void)out;
    /* No /sys/firmware/dmi outside Linux. The portable parser still
     * works on a buffer obtained some other way. */
    return -ENOTSUP;
}

#endif /* __linux__ */
