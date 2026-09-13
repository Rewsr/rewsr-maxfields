/*
 * test_cpuid.c, decoder tests against register captures from real
 * hardware.
 *
 * The EPYC values are from an AMD EPYC 7763 (Milan, family 0x19
 * model 0x01), the Xeon values from an Intel Xeon Platinum 8380
 * (Ice Lake SP, family 6 model 0x6A). Both match the publicly
 * archived InstLatx64 CPUID dumps for those parts, so a decoder
 * regression here means we would misreport real fleet hardware.
 */
#include <errno.h>
#include <string.h>

#include "../cpuid/rewsr_cpuid.h"
#include "ctest.h"

/* AMD EPYC 7763 captures. */
static const struct rewsr_cpuid_regs k_epyc_leaf0 = {
    0x00000010, 0x68747541, 0x444D4163, 0x69746E65,
};
static const struct rewsr_cpuid_regs k_epyc_leaf1 = {
    0x00A00F11, 0x00800800, 0x7EF8320B, 0x178BFBFF,
};
static const struct rewsr_cpuid_regs k_epyc_leaf7 = {
    0x00000000, 0x219C97A9, 0x0040069C, 0x00000010,
};
/* Leaf 0xB with SMT enabled: 2 threads/core, 128 logical/package. */
static const struct rewsr_cpuid_regs k_epyc_topo[3] = {
    { 0x00000001, 0x00000002, 0x00000100, 0x00000000 },
    { 0x00000007, 0x00000080, 0x00000201, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000002, 0x00000000 },
};
/* Brand string "AMD EPYC 7763 64-Core Processor", bytes spread over
 * leaves 0x80000002..4 in EAX,EBX,ECX,EDX order, LE per register. */
static const struct rewsr_cpuid_regs k_epyc_brand[3] = {
    { 0x20444D41, 0x43595045, 0x36373720, 0x34362033 },
    { 0x726F432D, 0x72502065, 0x7365636F, 0x00726F73 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
};

/* Intel Xeon Platinum 8380 captures. */
static const struct rewsr_cpuid_regs k_icx_leaf0 = {
    0x0000001B, 0x756E6547, 0x6C65746E, 0x49656E69,
};
static const struct rewsr_cpuid_regs k_icx_leaf1 = {
    0x000606A6, 0x00400800, 0x7FFEFBF7, 0xBFEBFBFF,
};
static const struct rewsr_cpuid_regs k_icx_leaf7 = {
    0x00000000, 0xF3BFA7EB, 0x40417F5E, 0xBC000400,
};
/* 2 threads/core, 80 logical (40 cores) per package. */
static const struct rewsr_cpuid_regs k_icx_topo[3] = {
    { 0x00000001, 0x00000002, 0x00000100, 0x00000000 },
    { 0x00000007, 0x00000050, 0x00000201, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000002, 0x00000000 },
};

static void test_vendor_strings(void) {
    char v[13];
    rewsr_cpuid_vendor(&k_epyc_leaf0, v);
    ASSERT_EQ_STR("AuthenticAMD", v);
    ASSERT_EQ_INT(REWSR_CPU_VENDOR_AMD, rewsr_cpuid_vendor_id(&k_epyc_leaf0));

    rewsr_cpuid_vendor(&k_icx_leaf0, v);
    ASSERT_EQ_STR("GenuineIntel", v);
    ASSERT_EQ_INT(REWSR_CPU_VENDOR_INTEL,
                  rewsr_cpuid_vendor_id(&k_icx_leaf0));

    struct rewsr_cpuid_regs junk = { 0, 0x11111111, 0x33333333,
                                     0x22222222 };
    ASSERT_EQ_INT(REWSR_CPU_VENDOR_UNKNOWN, rewsr_cpuid_vendor_id(&junk));
}

static void test_signature_epyc_milan(void) {
    struct rewsr_cpuid_signature sig;
    rewsr_cpuid_signature_decode(k_epyc_leaf1.eax, &sig);
    /* 0x00A00F11: base family 0xF + ext 0x0A = 0x19 (Zen3). */
    ASSERT_EQ_INT(0x19, sig.family);
    ASSERT_EQ_INT(0x01, sig.model);
    ASSERT_EQ_INT(1, sig.stepping);
    ASSERT_EQ_INT(0xF, sig.base_family);
    ASSERT_EQ_INT(0x0A, sig.ext_family);
}

static void test_signature_icelake(void) {
    struct rewsr_cpuid_signature sig;
    rewsr_cpuid_signature_decode(k_icx_leaf1.eax, &sig);
    /* 0x000606A6: family 6, model 0xA | (6 << 4) = 0x6A. */
    ASSERT_EQ_INT(6, sig.family);
    ASSERT_EQ_INT(0x6A, sig.model);
    ASSERT_EQ_INT(6, sig.stepping);
}

static void test_signature_fixup_corners(void) {
    struct rewsr_cpuid_signature sig;

    /* Pentium 4 Northwood, 0x00000F29: family 0xF with zero extended
     * family still lands on 0xF, model does merge (family >= 6). */
    rewsr_cpuid_signature_decode(0x00000F29, &sig);
    ASSERT_EQ_INT(0xF, sig.family);
    ASSERT_EQ_INT(0x2, sig.model);
    ASSERT_EQ_INT(9, sig.stepping);

    /* AMD K10 Opteron, 0x00100F42: 0xF + ext family 1 = 0x10. */
    rewsr_cpuid_signature_decode(0x00100F42, &sig);
    ASSERT_EQ_INT(0x10, sig.family);
    ASSERT_EQ_INT(0x4, sig.model);
    ASSERT_EQ_INT(2, sig.stepping);

    /* Sapphire Rapids, 0x000806F8: ext model prepends onto base. */
    rewsr_cpuid_signature_decode(0x000806F8, &sig);
    ASSERT_EQ_INT(6, sig.family);
    ASSERT_EQ_INT(0x8F, sig.model);
    ASSERT_EQ_INT(8, sig.stepping);

    /* Family 5 (original Pentium): no extended-model merge below
     * family 6, the raw base model must survive untouched. */
    rewsr_cpuid_signature_decode(0x0001052C, &sig);
    ASSERT_EQ_INT(5, sig.family);
    ASSERT_EQ_INT(0x2, sig.model);
    ASSERT_EQ_INT(0xC, sig.stepping);
}

static void test_features_epyc(void) {
    struct rewsr_cpu_features f;
    rewsr_cpuid_features_decode(k_epyc_leaf1.ecx, k_epyc_leaf1.edx,
                                k_epyc_leaf7.ebx, k_epyc_leaf7.ecx, &f);
    ASSERT_TRUE(f.sse && f.sse2 && f.sse3 && f.ssse3);
    ASSERT_TRUE(f.sse4_1 && f.sse4_2);
    ASSERT_TRUE(f.avx && f.avx2 && f.fma);
    ASSERT_TRUE(f.aes && f.pclmulqdq && f.sha);
    ASSERT_TRUE(f.rdrand && f.rdseed);
    ASSERT_TRUE(f.smep && f.smap);
    ASSERT_TRUE(f.clwb && f.clflushopt);
    ASSERT_TRUE(f.vaes && f.vpclmulqdq && f.rdpid);
    ASSERT_TRUE(f.popcnt && f.movbe && f.x2apic && f.cx16);
    ASSERT_TRUE(f.htt && f.tsc && f.msr);
    /* Milan explicitly lacks these. */
    ASSERT_TRUE(!f.avx512f);
    ASSERT_TRUE(!f.avx512bw);
    ASSERT_TRUE(!f.gfni);
    /* Bare metal capture, no VMM bit. */
    ASSERT_TRUE(!f.hypervisor);
}

static void test_features_icelake(void) {
    struct rewsr_cpu_features f;
    rewsr_cpuid_features_decode(k_icx_leaf1.ecx, k_icx_leaf1.edx,
                                k_icx_leaf7.ebx, k_icx_leaf7.ecx, &f);
    ASSERT_TRUE(f.avx512f && f.avx512dq && f.avx512cd);
    ASSERT_TRUE(f.avx512bw && f.avx512vl);
    ASSERT_TRUE(f.avx512vbmi && f.avx512vbmi2 && f.avx512vnni);
    ASSERT_TRUE(f.avx512bitalg && f.avx512vpopcntdq && f.avx512ifma);
    ASSERT_TRUE(f.gfni && f.vaes && f.vpclmulqdq);
    ASSERT_TRUE(f.sha && f.aes);
    ASSERT_TRUE(f.avx && f.avx2 && f.fma && f.f16c);
    ASSERT_TRUE(f.rdrand && f.rdseed && f.rdpid);
    ASSERT_TRUE(f.bmi1 && f.bmi2 && f.adx);
    ASSERT_TRUE(f.erms && f.invpcid && f.fsgsbase);
    ASSERT_TRUE(f.clwb && f.clflushopt);
    ASSERT_TRUE(!f.hypervisor);
}

static void test_features_string_order_and_truncation(void) {
    struct rewsr_cpu_features f;
    memset(&f, 0, sizeof(f));
    f.sse4_2 = 1;
    f.avx2 = 1;
    f.avx512f = 1;

    char buf[64];
    size_t need = rewsr_cpuid_features_string(&f, buf, sizeof(buf));
    ASSERT_EQ_STR("sse4_2 avx2 avx512f", buf);
    ASSERT_EQ_U64(strlen("sse4_2 avx2 avx512f"), need);

    /* Truncation never splits a name and still reports the full
     * length so callers can size a retry. */
    char tiny[8];
    need = rewsr_cpuid_features_string(&f, tiny, sizeof(tiny));
    ASSERT_EQ_STR("sse4_2", tiny);
    ASSERT_EQ_U64(strlen("sse4_2 avx2 avx512f"), need);

    /* cap 0 must not touch the buffer. */
    need = rewsr_cpuid_features_string(&f, NULL, 0);
    ASSERT_EQ_U64(strlen("sse4_2 avx2 avx512f"), need);
}

static void test_brand_string_epyc(void) {
    char out[49];
    rewsr_cpuid_brand_string(k_epyc_brand, out);
    ASSERT_EQ_STR("AMD EPYC 7763 64-Core Processor", out);
}

static void test_brand_string_icelake_built(void) {
    /* Build the ICX brand registers from the known string; the AMD
     * case above pins the register-to-byte order with hardcoded
     * constants, this one covers a full-length Intel string. */
    const char *brand = "Intel(R) Xeon(R) Platinum 8380 CPU @ 2.30GHz";
    uint8_t raw[48];
    memset(raw, 0, sizeof(raw));
    memcpy(raw, brand, strlen(brand));

    struct rewsr_cpuid_regs regs[3];
    for (int i = 0; i < 3; i++) {
        const uint8_t *p = raw + i * 16;
        regs[i].eax = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        regs[i].ebx = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        regs[i].ecx = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                      ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
        regs[i].edx = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                      ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    }
    char out[49];
    rewsr_cpuid_brand_string(regs, out);
    ASSERT_EQ_STR(brand, out);
}

static void test_brand_string_trims_padding(void) {
    /* Pre-Nehalem Intel firmware right-justifies the brand with
     * leading spaces; both ends must come off. */
    const char *padded = "      Intel(R) Xeon(R) CPU  X5570  @ 2.93GHz  ";
    uint8_t raw[48];
    memset(raw, 0, sizeof(raw));
    memcpy(raw, padded, strlen(padded));

    struct rewsr_cpuid_regs regs[3];
    for (int i = 0; i < 3; i++) {
        const uint8_t *p = raw + i * 16;
        regs[i].eax = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        regs[i].ebx = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        regs[i].ecx = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                      ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
        regs[i].edx = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                      ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    }
    char out[49];
    rewsr_cpuid_brand_string(regs, out);
    ASSERT_EQ_STR("Intel(R) Xeon(R) CPU  X5570  @ 2.93GHz", out);
}

static void test_topology_epyc(void) {
    struct rewsr_cpuid_topology topo;
    ASSERT_EQ_INT(0, rewsr_cpuid_topology_decode(k_epyc_topo, 3, &topo));
    ASSERT_TRUE(topo.valid);
    ASSERT_EQ_U64(2, topo.threads_per_core);
    ASSERT_EQ_U64(128, topo.logical_per_package);
    ASSERT_EQ_U64(64, topo.cores_per_package);
}

static void test_topology_icelake(void) {
    struct rewsr_cpuid_topology topo;
    ASSERT_EQ_INT(0, rewsr_cpuid_topology_decode(k_icx_topo, 3, &topo));
    ASSERT_TRUE(topo.valid);
    ASSERT_EQ_U64(2, topo.threads_per_core);
    ASSERT_EQ_U64(80, topo.logical_per_package);
    ASSERT_EQ_U64(40, topo.cores_per_package);
}

static void test_topology_smt_disabled(void) {
    /* SMT off in firmware: the SMT level reports width 1. */
    const struct rewsr_cpuid_regs no_smt[3] = {
        { 0x00000001, 0x00000001, 0x00000100, 0x00000000 },
        { 0x00000007, 0x00000040, 0x00000201, 0x00000000 },
        { 0x00000000, 0x00000000, 0x00000002, 0x00000000 },
    };
    struct rewsr_cpuid_topology topo;
    ASSERT_EQ_INT(0, rewsr_cpuid_topology_decode(no_smt, 3, &topo));
    ASSERT_TRUE(topo.valid);
    ASSERT_EQ_U64(1, topo.threads_per_core);
    ASSERT_EQ_U64(64, topo.cores_per_package);
}

static void test_topology_absent_leaf(void) {
    /* Old CPU without leaf 0xB: subleaf 0 comes back all zeros
     * (level type invalid), the decode must report not-valid rather
     * than inventing counts. */
    const struct rewsr_cpuid_regs empty[1] = {
        { 0, 0, 0, 0 },
    };
    struct rewsr_cpuid_topology topo;
    ASSERT_EQ_INT(0, rewsr_cpuid_topology_decode(empty, 1, &topo));
    ASSERT_TRUE(!topo.valid);
    ASSERT_EQ_INT(0, rewsr_cpuid_topology_decode(NULL, 0, &topo));
    ASSERT_TRUE(!topo.valid);
}

static void test_decode_snapshot_epyc(void) {
    struct rewsr_cpuid_snapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.leaf0 = k_epyc_leaf0;
    snap.leaf1 = k_epyc_leaf1;
    snap.leaf7_0 = k_epyc_leaf7;
    memcpy(snap.brand, k_epyc_brand, sizeof(k_epyc_brand));
    memcpy(snap.topo, k_epyc_topo, sizeof(k_epyc_topo));
    snap.topo_count = 3;
    snap.max_leaf = k_epyc_leaf0.eax;
    snap.max_ext_leaf = 0x80000023;

    struct rewsr_cpu_info info;
    rewsr_cpuid_decode_snapshot(&snap, &info);
    ASSERT_EQ_STR("AuthenticAMD", info.vendor);
    ASSERT_EQ_INT(REWSR_CPU_VENDOR_AMD, info.vendor_id);
    ASSERT_EQ_STR("AMD EPYC 7763 64-Core Processor", info.brand);
    ASSERT_EQ_INT(0x19, info.sig.family);
    ASSERT_EQ_INT(0x01, info.sig.model);
    ASSERT_TRUE(info.features.avx2);
    ASSERT_TRUE(!info.features.avx512f);
    ASSERT_EQ_U64(64, info.topology.cores_per_package);
}

static void test_native_cpuid_platform_behavior(void) {
    struct rewsr_cpuid_regs r;
    int rc = rewsr_cpuid(0, 0, &r);
#if defined(__x86_64__) || defined(__i386__)
    ASSERT_EQ_INT(0, rc);
    /* Any real x86 reports a nonzero max leaf. */
    ASSERT_TRUE(r.eax != 0);
    struct rewsr_cpuid_snapshot snap;
    ASSERT_EQ_INT(0, rewsr_cpuid_snapshot_collect(&snap));
    ASSERT_TRUE(snap.max_leaf != 0);
#else
    ASSERT_EQ_INT(-ENOTSUP, rc);
    ASSERT_EQ_U64(0, r.eax);
    struct rewsr_cpuid_snapshot snap;
    ASSERT_EQ_INT(-ENOTSUP, rewsr_cpuid_snapshot_collect(&snap));
#endif
}

REWSR_TEST_MAIN(
    RUN_TEST(test_vendor_strings);
    RUN_TEST(test_signature_epyc_milan);
    RUN_TEST(test_signature_icelake);
    RUN_TEST(test_signature_fixup_corners);
    RUN_TEST(test_features_epyc);
    RUN_TEST(test_features_icelake);
    RUN_TEST(test_features_string_order_and_truncation);
    RUN_TEST(test_brand_string_epyc);
    RUN_TEST(test_brand_string_icelake_built);
    RUN_TEST(test_brand_string_trims_padding);
    RUN_TEST(test_topology_epyc);
    RUN_TEST(test_topology_icelake);
    RUN_TEST(test_topology_smt_disabled);
    RUN_TEST(test_topology_absent_leaf);
    RUN_TEST(test_decode_snapshot_epyc);
    RUN_TEST(test_native_cpuid_platform_behavior);
)
