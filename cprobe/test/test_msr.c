/*
 * test_msr.c, decoder tests against raw MSR values captured from
 * real parts, plus platform pinning of the read path.
 */
#include <errno.h>
#include <math.h>

#include "../msr/rewsr_msr.h"
#include "ctest.h"

#define ASSERT_NEAR(expected, actual)                                      \
    ASSERT_TRUE(fabs((expected) - (actual)) < 1e-9)

static void test_rapl_units_intel(void) {
    /* MSR_RAPL_POWER_UNIT = 0x000A0E03 as read on Ice Lake SP:
     * power exponent 3 (1/8 W), energy exponent 14 (61.04 uJ), time
     * exponent 10 (976.6 us). */
    struct rewsr_rapl_units u;
    rewsr_msr_decode_rapl_units(0x000A0E03ull, &u);
    ASSERT_EQ_U64(8, u.power_unit_div);
    ASSERT_EQ_U64(16384, u.energy_unit_div);
    ASSERT_EQ_U64(1024, u.time_unit_div);
}

static void test_rapl_units_amd(void) {
    /* MSR_AMD_RAPL_POWER_UNIT on EPYC reports energy exponent 16
     * (15.26 uJ per tick): raw 0x000A1003. */
    struct rewsr_rapl_units u;
    rewsr_msr_decode_rapl_units(0x000A1003ull, &u);
    ASSERT_EQ_U64(8, u.power_unit_div);
    ASSERT_EQ_U64(65536, u.energy_unit_div);
    ASSERT_EQ_U64(1024, u.time_unit_div);
}

static void test_energy_joules(void) {
    struct rewsr_rapl_units u;
    rewsr_msr_decode_rapl_units(0x000A0E03ull, &u); /* 1/16384 J */

    ASSERT_NEAR(10000.0, rewsr_msr_energy_joules(163840000ull, &u));
    ASSERT_NEAR(0.0, rewsr_msr_energy_joules(0, &u));
    /* Reserved high half of the register must be ignored. */
    ASSERT_NEAR(1.0,
                rewsr_msr_energy_joules(0xDEADBEEF00000000ull | 16384, &u));
}

static void test_energy_delta_wraps(void) {
    struct rewsr_rapl_units u;
    rewsr_msr_decode_rapl_units(0x000A0E03ull, &u);

    ASSERT_NEAR(1.0, rewsr_msr_energy_delta_joules(0, 16384, &u));
    /* Counter wrapped between samples: 2^32 - 16384 -> 16384 is
     * exactly 2 J of accumulation. */
    ASSERT_NEAR(2.0, rewsr_msr_energy_delta_joules(0xFFFFC000u, 0x4000u,
                                                   &u));
}

static void test_tsc_delta(void) {
    /* 3000 ticks at 1 GHz = 3000 ns. */
    ASSERT_EQ_U64(3000, rewsr_msr_tsc_delta_ns(1000, 4000, 1000000000ull));
    /* 4900 ticks at 2.45 GHz = 2000 ns. */
    ASSERT_EQ_U64(2000, rewsr_msr_tsc_delta_ns(0, 4900, 2450000000ull));
    /* Wrap across 2^64. */
    ASSERT_EQ_U64(3000,
                  rewsr_msr_tsc_delta_ns(UINT64_MAX - 999, 2000,
                                         1000000000ull));
    /* Long interval that would overflow 64-bit ticks*1e9: one hour at
     * 3 GHz. */
    ASSERT_EQ_U64(3600000000000ull,
                  rewsr_msr_tsc_delta_ns(0, 3600ull * 3000000000ull,
                                         3000000000ull));
    ASSERT_EQ_U64(0, rewsr_msr_tsc_delta_ns(0, 100, 0));
}

static void test_platform_info(void) {
    /* Xeon Platinum 8380: base 2.3 GHz, ratio 0x17 in bits 15:8.
     * Live register reads 0x80838f3012300 style values; only the
     * ratio byte matters here. */
    ASSERT_EQ_U64(2300,
                  rewsr_msr_platform_info_base_mhz(0x0000000000001700ull));
    ASSERT_EQ_U64(2400,
                  rewsr_msr_platform_info_base_mhz(0x00808F3012301800ull));
    ASSERT_EQ_U64(0, rewsr_msr_platform_info_base_mhz(0));
}

static void test_therm_status(void) {
    int c = 0;
    /* Readout 20 below a TjMax of 100 = 80 C. */
    uint64_t therm = (1ull << 31) | (20ull << 16);
    uint64_t target = 100ull << 16;
    ASSERT_EQ_INT(0, rewsr_msr_therm_celsius(therm, target, &c));
    ASSERT_EQ_INT(80, c);

    /* Idle box: readout 61 below TjMax 90. */
    therm = (1ull << 31) | (61ull << 16);
    target = 90ull << 16;
    ASSERT_EQ_INT(0, rewsr_msr_therm_celsius(therm, target, &c));
    ASSERT_EQ_INT(29, c);

    /* Valid bit clear. */
    ASSERT_EQ_INT(-ENODATA,
                  rewsr_msr_therm_celsius(20ull << 16, target, &c));
    /* Unreadable TjMax. */
    ASSERT_EQ_INT(-ENODATA,
                  rewsr_msr_therm_celsius((1ull << 31) | (20ull << 16), 0,
                                          &c));
}

static void test_read_platform_behavior(void) {
    uint64_t v = 0;
    ASSERT_EQ_INT(-EINVAL, rewsr_msr_read(-1, REWSR_MSR_IA32_TSC, &v));
#if defined(__linux__)
    /* On Linux the outcome depends on the environment: success with
     * the msr module and privilege, -ENOENT without the module,
     * -EACCES unprivileged. All are legitimate; a crash or a bogus
     * errno is not. */
    int rc = rewsr_msr_read(0, REWSR_MSR_IA32_TSC, &v);
    ASSERT_TRUE(rc == 0 || rc == -ENOENT || rc == -EACCES ||
                rc == -EPERM || rc == -EIO);
    if (rc == 0) {
        ASSERT_TRUE(v != 0); /* the TSC of a running CPU is not 0 */
    }
#else
    ASSERT_EQ_INT(-ENOTSUP, rewsr_msr_read(0, REWSR_MSR_IA32_TSC, &v));
#endif
}

REWSR_TEST_MAIN(
    RUN_TEST(test_rapl_units_intel);
    RUN_TEST(test_rapl_units_amd);
    RUN_TEST(test_energy_joules);
    RUN_TEST(test_energy_delta_wraps);
    RUN_TEST(test_tsc_delta);
    RUN_TEST(test_platform_info);
    RUN_TEST(test_therm_status);
    RUN_TEST(test_read_platform_behavior);
)
