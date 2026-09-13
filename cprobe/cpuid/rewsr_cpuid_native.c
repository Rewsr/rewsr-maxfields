/*
 * rewsr_cpuid_native.c, the only file that executes CPUID.
 *
 * Inline asm instead of <cpuid.h> so the same source builds with any
 * compiler that targets x86 and syntax-checks everywhere else. On
 * non-x86 hosts every entry point reports -ENOTSUP; the portable
 * decoders in rewsr_cpuid.c still work there on captured registers.
 */
#include "rewsr_cpuid.h"

#include <errno.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__)

int rewsr_cpuid(uint32_t leaf, uint32_t subleaf,
                struct rewsr_cpuid_regs *out) {
    uint32_t a, b, c, d;
#if defined(__i386__) && defined(__PIC__)
    /* On PIC i386, EBX is the GOT pointer and cannot be clobbered
     * directly; stash it around the instruction. Not a build we ship,
     * but it costs nothing to stay correct. */
    __asm__ volatile("xchgl %%ebx, %1\n\t"
                     "cpuid\n\t"
                     "xchgl %%ebx, %1"
                     : "=a"(a), "=r"(b), "=c"(c), "=d"(d)
                     : "0"(leaf), "2"(subleaf));
#else
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "0"(leaf), "2"(subleaf));
#endif
    out->eax = a;
    out->ebx = b;
    out->ecx = c;
    out->edx = d;
    return 0;
}

int rewsr_cpuid_snapshot_collect(struct rewsr_cpuid_snapshot *out) {
    memset(out, 0, sizeof(*out));

    rewsr_cpuid(0, 0, &out->leaf0);
    out->max_leaf = out->leaf0.eax;

    if (out->max_leaf >= 1) {
        rewsr_cpuid(1, 0, &out->leaf1);
    }
    if (out->max_leaf >= 7) {
        rewsr_cpuid(7, 0, &out->leaf7_0);
    }

    struct rewsr_cpuid_regs ext0;
    rewsr_cpuid(0x80000000u, 0, &ext0);
    out->max_ext_leaf = ext0.eax;
    if (out->max_ext_leaf >= 0x80000004u) {
        rewsr_cpuid(0x80000002u, 0, &out->brand[0]);
        rewsr_cpuid(0x80000003u, 0, &out->brand[1]);
        rewsr_cpuid(0x80000004u, 0, &out->brand[2]);
    }

    if (out->max_leaf >= 0xB) {
        for (uint32_t sl = 0; sl < REWSR_CPUID_MAX_TOPO_LEVELS; sl++) {
            struct rewsr_cpuid_regs r;
            rewsr_cpuid(0xB, sl, &r);
            uint32_t type = (r.ecx >> 8) & 0xFF;
            out->topo[sl] = r;
            out->topo_count = sl + 1;
            if (type == REWSR_CPUID_TOPO_INVALID) {
                break;
            }
        }
    }
    return 0;
}

#else /* not x86 */

int rewsr_cpuid(uint32_t leaf, uint32_t subleaf,
                struct rewsr_cpuid_regs *out) {
    (void)leaf;
    (void)subleaf;
    memset(out, 0, sizeof(*out));
    return -ENOTSUP;
}

int rewsr_cpuid_snapshot_collect(struct rewsr_cpuid_snapshot *out) {
    memset(out, 0, sizeof(*out));
    return -ENOTSUP;
}

#endif /* x86 */
