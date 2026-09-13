/*
 * rewsr_cpuid.h, x86 CPUID capture and decode.
 *
 * Split on purpose: rewsr_cpuid() and rewsr_cpuid_snapshot_collect()
 * execute the instruction and only exist on x86 (rewsr_cpuid_native.c),
 * everything else decodes raw register values and is portable, so the
 * decoders are tested on the arm64 dev box against register captures
 * taken from real EPYC and Xeon parts.
 *
 * Register conventions used below:
 *   leaf 0            EAX = max standard leaf, EBX:EDX:ECX spell the
 *                     12 byte vendor string in that order
 *   leaf 1            EAX = family/model/stepping signature,
 *                     ECX/EDX = feature bits
 *   leaf 7 subleaf 0  EBX/ECX = extended feature bits
 *   leaf 0xB          topology enumeration, one subleaf per level
 *   0x80000002..4     brand string, 16 bytes per leaf in EAX,EBX,ECX,EDX
 */
#ifndef REWSR_CPUID_H
#define REWSR_CPUID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rewsr_cpuid_regs {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
};

/*
 * Execute CPUID with the given leaf/subleaf. Returns 0 and fills
 * *out on x86, -ENOTSUP elsewhere (out is zeroed so callers cannot
 * consume stack garbage).
 */
int rewsr_cpuid(uint32_t leaf, uint32_t subleaf,
                struct rewsr_cpuid_regs *out);

/* Enough levels for SMT + core + module/tile/die on current parts. */
#define REWSR_CPUID_MAX_TOPO_LEVELS 6

/*
 * Raw capture of every leaf the decoders below consume. Collect once,
 * decode portably. Also the natural serialization boundary if the Go
 * side ever wants to ship raw captures home.
 */
struct rewsr_cpuid_snapshot {
    struct rewsr_cpuid_regs leaf0;
    struct rewsr_cpuid_regs leaf1;
    struct rewsr_cpuid_regs leaf7_0;   /* leaf 7, subleaf 0 */
    struct rewsr_cpuid_regs brand[3];  /* 0x80000002..0x80000004 */
    struct rewsr_cpuid_regs topo[REWSR_CPUID_MAX_TOPO_LEVELS]; /* leaf 0xB */
    size_t topo_count;
    uint32_t max_leaf;
    uint32_t max_ext_leaf;             /* EAX of leaf 0x80000000 */
};

/* x86 only, -ENOTSUP elsewhere. */
int rewsr_cpuid_snapshot_collect(struct rewsr_cpuid_snapshot *out);

/* Vendor identification. */
enum rewsr_cpu_vendor {
    REWSR_CPU_VENDOR_UNKNOWN = 0,
    REWSR_CPU_VENDOR_INTEL,
    REWSR_CPU_VENDOR_AMD,
};

/*
 * Spell out the leaf 0 vendor string. out must hold 13 bytes and is
 * always NUL-terminated. The register-to-byte order is EBX, EDX, ECX
 * (yes, EDX in the middle; that is the architected order).
 */
void rewsr_cpuid_vendor(const struct rewsr_cpuid_regs *leaf0, char out[13]);

enum rewsr_cpu_vendor
rewsr_cpuid_vendor_id(const struct rewsr_cpuid_regs *leaf0);

/*
 * Family/model/stepping from the leaf 1 EAX signature dword:
 *   bits  3:0  stepping
 *   bits  7:4  base model
 *   bits 11:8  base family
 *   bits 13:12 processor type
 *   bits 19:16 extended model
 *   bits 27:20 extended family
 * Fixups follow the Linux kernel (arch/x86/kernel/cpu/common.c):
 * family += extended family when base family == 0xF, and the extended
 * model prepends onto the base model whenever base family >= 6. This
 * matches how both vendors document their parts (EPYC Milan is family
 * 0x19 = 0xF + 0xA, Ice Lake SP is model 0x6A = 0xA | 6 << 4).
 */
struct rewsr_cpuid_signature {
    uint16_t family;
    uint16_t model;
    uint8_t stepping;
    uint8_t base_family;
    uint8_t base_model;
    uint8_t ext_family;
    uint8_t ext_model;
};

void rewsr_cpuid_signature_decode(uint32_t leaf1_eax,
                                  struct rewsr_cpuid_signature *out);

/*
 * Feature flags the facts collector cares about. Each field is 0 or 1.
 * Sourced from leaf 1 ECX/EDX and leaf 7 subleaf 0 EBX/ECX; the bit
 * positions are listed next to each flag and are architectural.
 */
struct rewsr_cpu_features {
    /* leaf 1 EDX */
    uint8_t fpu;        /* bit 0 */
    uint8_t tsc;        /* bit 4 */
    uint8_t msr;        /* bit 5 */
    uint8_t cx8;        /* bit 8 */
    uint8_t sep;        /* bit 11 */
    uint8_t cmov;       /* bit 15 */
    uint8_t clfsh;      /* bit 19 */
    uint8_t mmx;        /* bit 23 */
    uint8_t sse;        /* bit 25 */
    uint8_t sse2;       /* bit 26 */
    uint8_t htt;        /* bit 28 */

    /* leaf 1 ECX */
    uint8_t sse3;       /* bit 0 */
    uint8_t pclmulqdq;  /* bit 1 */
    uint8_t ssse3;      /* bit 9 */
    uint8_t fma;        /* bit 12 */
    uint8_t cx16;       /* bit 13 */
    uint8_t sse4_1;     /* bit 19 */
    uint8_t sse4_2;     /* bit 20 */
    uint8_t x2apic;     /* bit 21 */
    uint8_t movbe;      /* bit 22 */
    uint8_t popcnt;     /* bit 23 */
    uint8_t aes;        /* bit 25 */
    uint8_t xsave;      /* bit 26 */
    uint8_t osxsave;    /* bit 27 */
    uint8_t avx;        /* bit 28 */
    uint8_t f16c;       /* bit 29 */
    uint8_t rdrand;     /* bit 30 */
    uint8_t hypervisor; /* bit 31, set when running under a VMM */

    /* leaf 7 subleaf 0 EBX */
    uint8_t fsgsbase;    /* bit 0 */
    uint8_t bmi1;        /* bit 3 */
    uint8_t avx2;        /* bit 5 */
    uint8_t smep;        /* bit 7 */
    uint8_t bmi2;        /* bit 8 */
    uint8_t erms;        /* bit 9 */
    uint8_t invpcid;     /* bit 10 */
    uint8_t avx512f;     /* bit 16 */
    uint8_t avx512dq;    /* bit 17 */
    uint8_t rdseed;      /* bit 18 */
    uint8_t adx;         /* bit 19 */
    uint8_t smap;        /* bit 20 */
    uint8_t avx512ifma;  /* bit 21 */
    uint8_t clflushopt;  /* bit 23 */
    uint8_t clwb;        /* bit 24 */
    uint8_t avx512cd;    /* bit 28 */
    uint8_t sha;         /* bit 29 */
    uint8_t avx512bw;    /* bit 30 */
    uint8_t avx512vl;    /* bit 31 */

    /* leaf 7 subleaf 0 ECX */
    uint8_t avx512vbmi;      /* bit 1 */
    uint8_t avx512vbmi2;     /* bit 6 */
    uint8_t gfni;            /* bit 8 */
    uint8_t vaes;            /* bit 9 */
    uint8_t vpclmulqdq;      /* bit 10 */
    uint8_t avx512vnni;      /* bit 11 */
    uint8_t avx512bitalg;    /* bit 12 */
    uint8_t avx512vpopcntdq; /* bit 14 */
    uint8_t rdpid;           /* bit 22 */
};

void rewsr_cpuid_features_decode(uint32_t leaf1_ecx, uint32_t leaf1_edx,
                                 uint32_t leaf7_ebx, uint32_t leaf7_ecx,
                                 struct rewsr_cpu_features *out);

/*
 * Render the set flags as a space-separated list in /proc/cpuinfo
 * spelling ("sse4_2 avx2 avx512f ..."). Returns the number of bytes
 * that would have been written (snprintf convention) so callers can
 * detect truncation; buf is always NUL-terminated when cap > 0.
 */
size_t rewsr_cpuid_features_string(const struct rewsr_cpu_features *f,
                                   char *buf, size_t cap);

/*
 * Brand string from leaves 0x80000002..0x80000004. brand[i] holds the
 * registers of leaf 0x80000002 + i; bytes come out in EAX,EBX,ECX,EDX
 * order, little-endian within each register. out must hold 49 bytes.
 * Intel left-pads with spaces, so leading and trailing whitespace is
 * trimmed.
 */
void rewsr_cpuid_brand_string(const struct rewsr_cpuid_regs brand[3],
                              char out[49]);

/*
 * Topology from leaf 0xB. Per subleaf:
 *   EAX bits 4:0   shift to strip this level from the x2APIC ID
 *   EBX bits 15:0  logical processors at this level (per package)
 *   ECX bits 15:8  level type: 0 invalid, 1 SMT, 2 core
 * The list terminates at the first invalid level. Pass the subleaves
 * in order starting at 0.
 */
#define REWSR_CPUID_TOPO_INVALID 0
#define REWSR_CPUID_TOPO_SMT     1
#define REWSR_CPUID_TOPO_CORE    2

struct rewsr_cpuid_topology {
    uint32_t threads_per_core;
    uint32_t logical_per_package;
    uint32_t cores_per_package;
    int valid; /* 0 when leaf 0xB was empty or absent */
};

int rewsr_cpuid_topology_decode(const struct rewsr_cpuid_regs *subleaves,
                                size_t count,
                                struct rewsr_cpuid_topology *out);

/*
 * Full portable decode of a snapshot. Pure function of its input, so
 * it runs anywhere; on a real x86 box pair it with
 * rewsr_cpuid_snapshot_collect().
 */
struct rewsr_cpu_info {
    char vendor[13];
    enum rewsr_cpu_vendor vendor_id;
    char brand[49];
    struct rewsr_cpuid_signature sig;
    struct rewsr_cpu_features features;
    struct rewsr_cpuid_topology topology;
};

void rewsr_cpuid_decode_snapshot(const struct rewsr_cpuid_snapshot *snap,
                                 struct rewsr_cpu_info *out);

#ifdef __cplusplus
}
#endif

#endif /* REWSR_CPUID_H */
