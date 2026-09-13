/*
 * rewsr_pci_linux.c, live-system entry point for the PCI scan.
 *
 * All the logic lives in the portable rewsr_pci_scan_root(); this
 * wrapper only pins the root to the real filesystem, and only on
 * Linux, because /sys/bus/pci does not exist anywhere else.
 */
#include "rewsr_pci.h"

#include <errno.h>

#if defined(__linux__)

int rewsr_pci_scan(struct rewsr_pci_dev *out, size_t cap, size_t *found) {
    return rewsr_pci_scan_root("", out, cap, found);
}

#else /* !__linux__ */

int rewsr_pci_scan(struct rewsr_pci_dev *out, size_t cap, size_t *found) {
    (void)out;
    (void)cap;
    if (found != NULL) {
        *found = 0;
    }
    return -ENOTSUP;
}

#endif /* __linux__ */
