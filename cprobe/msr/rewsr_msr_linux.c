/*
 * rewsr_msr_linux.c, the privileged MSR read.
 *
 * /dev/cpu/N/msr semantics (Documentation/arch/x86 and
 * arch/x86/kernel/msr.c): the file offset selects the MSR number,
 * reads are always 8 bytes, and the read executes RDMSR on CPU N
 * regardless of which CPU we run on, so no affinity dance is needed.
 */
#include "rewsr_msr.h"

#include <errno.h>

#if defined(__linux__)

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int rewsr_msr_read(int cpu, uint32_t msr, uint64_t *out) {
    if (cpu < 0 || out == NULL) {
        return -EINVAL;
    }
    char path[64];
    int n = snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -EINVAL;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    uint64_t v = 0;
    ssize_t r;
    do {
        r = pread(fd, &v, sizeof(v), (off_t)msr);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        int saved = errno;
        close(fd);
        return -saved;
    }
    close(fd);
    if (r != (ssize_t)sizeof(v)) {
        return -EIO;
    }
    *out = v;
    return 0;
}

#else /* !__linux__ */

int rewsr_msr_read(int cpu, uint32_t msr, uint64_t *out) {
    (void)cpu;
    (void)msr;
    if (out != NULL) {
        *out = 0;
    }
    /* No /dev/cpu/N/msr outside Linux; macOS has no unprivileged MSR
     * path at all. */
    return -ENOTSUP;
}

#endif /* __linux__ */
