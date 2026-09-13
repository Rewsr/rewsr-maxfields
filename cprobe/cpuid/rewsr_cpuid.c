/*
 * rewsr_cpuid.c, portable CPUID register decoders.
 *
 * Nothing in this file executes CPUID; it turns raw register values
 * into facts. That keeps every branch testable on non-x86 hosts with
 * hardcoded captures from real parts (see test/test_cpuid.c).
 */
#include "rewsr_cpuid.h"

#include <stdio.h>
#include <string.h>

static void put_le32(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

void rewsr_cpuid_vendor(const struct rewsr_cpuid_regs *leaf0,
                        char out[13]) {
    /* Architected order is EBX, EDX, ECX: "Genu" "ineI" "ntel". */
    uint8_t raw[12];
    put_le32(raw + 0, leaf0->ebx);
    put_le32(raw + 4, leaf0->edx);
    put_le32(raw + 8, leaf0->ecx);
    memcpy(out, raw, 12);
    out[12] = '\0';
}

enum rewsr_cpu_vendor
rewsr_cpuid_vendor_id(const struct rewsr_cpuid_regs *leaf0) {
    char v[13];
    rewsr_cpuid_vendor(leaf0, v);
    if (strcmp(v, "GenuineIntel") == 0) {
        return REWSR_CPU_VENDOR_INTEL;
    }
    if (strcmp(v, "AuthenticAMD") == 0) {
        return REWSR_CPU_VENDOR_AMD;
    }
    /* Early K5 engineering silicon reported "AMDisbetter!"; nothing
     * in a datacenter does, so it stays unknown on purpose. */
    return REWSR_CPU_VENDOR_UNKNOWN;
}

void rewsr_cpuid_signature_decode(uint32_t leaf1_eax,
                                  struct rewsr_cpuid_signature *out) {
    out->stepping = (uint8_t)(leaf1_eax & 0xF);
    out->base_model = (uint8_t)((leaf1_eax >> 4) & 0xF);
    out->base_family = (uint8_t)((leaf1_eax >> 8) & 0xF);
    out->ext_model = (uint8_t)((leaf1_eax >> 16) & 0xF);
    out->ext_family = (uint8_t)((leaf1_eax >> 20) & 0xFF);

    /* Kernel-compatible fixups, see the header comment for why. */
    out->family = out->base_family;
    if (out->base_family == 0xF) {
        out->family = (uint16_t)(out->base_family + out->ext_family);
    }
    out->model = out->base_model;
    if (out->base_family >= 6) {
        out->model = (uint16_t)(out->base_model |
                                ((uint16_t)out->ext_model << 4));
    }
}

void rewsr_cpuid_features_decode(uint32_t leaf1_ecx, uint32_t leaf1_edx,
                                 uint32_t leaf7_ebx, uint32_t leaf7_ecx,
                                 struct rewsr_cpu_features *out) {
    memset(out, 0, sizeof(*out));

#define BIT(reg, n) (uint8_t)(((reg) >> (n)) & 1u)
    out->fpu = BIT(leaf1_edx, 0);
    out->tsc = BIT(leaf1_edx, 4);
    out->msr = BIT(leaf1_edx, 5);
    out->cx8 = BIT(leaf1_edx, 8);
    out->sep = BIT(leaf1_edx, 11);
    out->cmov = BIT(leaf1_edx, 15);
    out->clfsh = BIT(leaf1_edx, 19);
    out->mmx = BIT(leaf1_edx, 23);
    out->sse = BIT(leaf1_edx, 25);
    out->sse2 = BIT(leaf1_edx, 26);
    out->htt = BIT(leaf1_edx, 28);

    out->sse3 = BIT(leaf1_ecx, 0);
    out->pclmulqdq = BIT(leaf1_ecx, 1);
    out->ssse3 = BIT(leaf1_ecx, 9);
    out->fma = BIT(leaf1_ecx, 12);
    out->cx16 = BIT(leaf1_ecx, 13);
    out->sse4_1 = BIT(leaf1_ecx, 19);
    out->sse4_2 = BIT(leaf1_ecx, 20);
    out->x2apic = BIT(leaf1_ecx, 21);
    out->movbe = BIT(leaf1_ecx, 22);
    out->popcnt = BIT(leaf1_ecx, 23);
    out->aes = BIT(leaf1_ecx, 25);
    out->xsave = BIT(leaf1_ecx, 26);
    out->osxsave = BIT(leaf1_ecx, 27);
    out->avx = BIT(leaf1_ecx, 28);
    out->f16c = BIT(leaf1_ecx, 29);
    out->rdrand = BIT(leaf1_ecx, 30);
    out->hypervisor = BIT(leaf1_ecx, 31);

    out->fsgsbase = BIT(leaf7_ebx, 0);
    out->bmi1 = BIT(leaf7_ebx, 3);
    out->avx2 = BIT(leaf7_ebx, 5);
    out->smep = BIT(leaf7_ebx, 7);
    out->bmi2 = BIT(leaf7_ebx, 8);
    out->erms = BIT(leaf7_ebx, 9);
    out->invpcid = BIT(leaf7_ebx, 10);
    out->avx512f = BIT(leaf7_ebx, 16);
    out->avx512dq = BIT(leaf7_ebx, 17);
    out->rdseed = BIT(leaf7_ebx, 18);
    out->adx = BIT(leaf7_ebx, 19);
    out->smap = BIT(leaf7_ebx, 20);
    out->avx512ifma = BIT(leaf7_ebx, 21);
    out->clflushopt = BIT(leaf7_ebx, 23);
    out->clwb = BIT(leaf7_ebx, 24);
    out->avx512cd = BIT(leaf7_ebx, 28);
    out->sha = BIT(leaf7_ebx, 29);
    out->avx512bw = BIT(leaf7_ebx, 30);
    out->avx512vl = BIT(leaf7_ebx, 31);

    out->avx512vbmi = BIT(leaf7_ecx, 1);
    out->avx512vbmi2 = BIT(leaf7_ecx, 6);
    out->gfni = BIT(leaf7_ecx, 8);
    out->vaes = BIT(leaf7_ecx, 9);
    out->vpclmulqdq = BIT(leaf7_ecx, 10);
    out->avx512vnni = BIT(leaf7_ecx, 11);
    out->avx512bitalg = BIT(leaf7_ecx, 12);
    out->avx512vpopcntdq = BIT(leaf7_ecx, 14);
    out->rdpid = BIT(leaf7_ecx, 22);
#undef BIT
}

/*
 * Name table drives the string renderer. Order follows the struct so
 * the output is stable across runs, which matters because the Go side
 * diffs collected facts between runs to detect drift.
 */
struct feature_name {
    size_t offset;
    const char *name;
};

#define FEAT(field, label) { offsetof(struct rewsr_cpu_features, field), label }

static const struct feature_name k_feature_names[] = {
    FEAT(fpu, "fpu"),
    FEAT(tsc, "tsc"),
    FEAT(msr, "msr"),
    FEAT(cx8, "cx8"),
    FEAT(sep, "sep"),
    FEAT(cmov, "cmov"),
    FEAT(clfsh, "clflush"),
    FEAT(mmx, "mmx"),
    FEAT(sse, "sse"),
    FEAT(sse2, "sse2"),
    FEAT(htt, "ht"),
    FEAT(sse3, "pni"),
    FEAT(pclmulqdq, "pclmulqdq"),
    FEAT(ssse3, "ssse3"),
    FEAT(fma, "fma"),
    FEAT(cx16, "cx16"),
    FEAT(sse4_1, "sse4_1"),
    FEAT(sse4_2, "sse4_2"),
    FEAT(x2apic, "x2apic"),
    FEAT(movbe, "movbe"),
    FEAT(popcnt, "popcnt"),
    FEAT(aes, "aes"),
    FEAT(xsave, "xsave"),
    FEAT(osxsave, "osxsave"),
    FEAT(avx, "avx"),
    FEAT(f16c, "f16c"),
    FEAT(rdrand, "rdrand"),
    FEAT(hypervisor, "hypervisor"),
    FEAT(fsgsbase, "fsgsbase"),
    FEAT(bmi1, "bmi1"),
    FEAT(avx2, "avx2"),
    FEAT(smep, "smep"),
    FEAT(bmi2, "bmi2"),
    FEAT(erms, "erms"),
    FEAT(invpcid, "invpcid"),
    FEAT(avx512f, "avx512f"),
    FEAT(avx512dq, "avx512dq"),
    FEAT(rdseed, "rdseed"),
    FEAT(adx, "adx"),
    FEAT(smap, "smap"),
    FEAT(avx512ifma, "avx512ifma"),
    FEAT(clflushopt, "clflushopt"),
    FEAT(clwb, "clwb"),
    FEAT(avx512cd, "avx512cd"),
    FEAT(sha, "sha_ni"),
    FEAT(avx512bw, "avx512bw"),
    FEAT(avx512vl, "avx512vl"),
    FEAT(avx512vbmi, "avx512vbmi"),
    FEAT(avx512vbmi2, "avx512_vbmi2"),
    FEAT(gfni, "gfni"),
    FEAT(vaes, "vaes"),
    FEAT(vpclmulqdq, "vpclmulqdq"),
    FEAT(avx512vnni, "avx512_vnni"),
    FEAT(avx512bitalg, "avx512_bitalg"),
    FEAT(avx512vpopcntdq, "avx512_vpopcntdq"),
    FEAT(rdpid, "rdpid"),
};

#undef FEAT

size_t rewsr_cpuid_features_string(const struct rewsr_cpu_features *f,
                                   char *buf, size_t cap) {
    const uint8_t *base = (const uint8_t *)f;
    size_t need = 0;
    size_t used = 0;
    int first = 1;
    if (cap > 0) {
        buf[0] = '\0';
    }
    for (size_t i = 0;
         i < sizeof(k_feature_names) / sizeof(k_feature_names[0]); i++) {
        if (!base[k_feature_names[i].offset]) {
            continue;
        }
        size_t nlen = strlen(k_feature_names[i].name);
        size_t add = nlen + (first ? 0 : 1);
        need += add;
        if (cap > 0 && used + add < cap) {
            if (!first) {
                buf[used] = ' ';
                memcpy(buf + used + 1, k_feature_names[i].name, nlen);
            } else {
                memcpy(buf + used, k_feature_names[i].name, nlen);
            }
            used += add;
            buf[used] = '\0';
        }
        first = 0;
    }
    return need;
}

void rewsr_cpuid_brand_string(const struct rewsr_cpuid_regs brand[3],
                              char out[49]) {
    uint8_t raw[48];
    for (int i = 0; i < 3; i++) {
        put_le32(raw + i * 16 + 0, brand[i].eax);
        put_le32(raw + i * 16 + 4, brand[i].ebx);
        put_le32(raw + i * 16 + 8, brand[i].ecx);
        put_le32(raw + i * 16 + 12, brand[i].edx);
    }
    /* The 48 bytes are NUL-padded but not guaranteed NUL-terminated
     * when the string uses all 48; bound the copy ourselves. Intel
     * right-justifies with leading spaces on some families, trim both
     * ends. */
    size_t start = 0;
    while (start < 48 && (raw[start] == ' ')) {
        start++;
    }
    size_t end = start;
    size_t last_nonspace = start;
    while (end < 48 && raw[end] != '\0') {
        if (raw[end] != ' ') {
            last_nonspace = end + 1;
        }
        end++;
    }
    size_t n = last_nonspace > start ? last_nonspace - start : 0;
    memcpy(out, raw + start, n);
    out[n] = '\0';
}

int rewsr_cpuid_topology_decode(const struct rewsr_cpuid_regs *subleaves,
                                size_t count,
                                struct rewsr_cpuid_topology *out) {
    memset(out, 0, sizeof(*out));
    if (subleaves == NULL || count == 0) {
        return 0;
    }
    uint32_t smt_width = 0;
    uint32_t deepest_count = 0;
    int saw_level = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t type = (subleaves[i].ecx >> 8) & 0xFF;
        if (type == REWSR_CPUID_TOPO_INVALID) {
            break;
        }
        saw_level = 1;
        uint32_t nlog = subleaves[i].ebx & 0xFFFF;
        if (type == REWSR_CPUID_TOPO_SMT) {
            smt_width = nlog;
        }
        /* The deepest reported level counts all logical processors in
         * the package; levels are ordered from SMT outward, so keep
         * the last one. */
        if (nlog > deepest_count) {
            deepest_count = nlog;
        }
    }
    if (!saw_level) {
        return 0;
    }
    out->valid = 1;
    out->threads_per_core = smt_width ? smt_width : 1;
    out->logical_per_package = deepest_count ? deepest_count : 1;
    out->cores_per_package =
        out->logical_per_package / out->threads_per_core;
    return 0;
}

void rewsr_cpuid_decode_snapshot(const struct rewsr_cpuid_snapshot *snap,
                                 struct rewsr_cpu_info *out) {
    memset(out, 0, sizeof(*out));
    rewsr_cpuid_vendor(&snap->leaf0, out->vendor);
    out->vendor_id = rewsr_cpuid_vendor_id(&snap->leaf0);
    rewsr_cpuid_brand_string(snap->brand, out->brand);
    rewsr_cpuid_signature_decode(snap->leaf1.eax, &out->sig);
    rewsr_cpuid_features_decode(snap->leaf1.ecx, snap->leaf1.edx,
                                snap->leaf7_0.ebx, snap->leaf7_0.ecx,
                                &out->features);
    rewsr_cpuid_topology_decode(snap->topo, snap->topo_count,
                                &out->topology);
}
