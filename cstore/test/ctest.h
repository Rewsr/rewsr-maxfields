/* ctest.h: header-only test harness for cstore.
 *
 * Usage:
 *   static int test_x(void) { ASSERT_TRUE(1); return 0; }
 *   static void suite(void) { RUN_TEST(test_x); }
 *   REWSR_TEST_MAIN(suite)
 *
 * Test functions return 0 on success. ASSERT_* macros return 1 from the
 * enclosing test function on failure, so the rest of the suite still runs.
 * The generated main() exits nonzero if any test failed.
 */
#ifndef REWSR_CTEST_H
#define REWSR_CTEST_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int ctest_tests_run;
static int ctest_tests_failed;
static int ctest_asserts;

#define CTEST_FAIL_AT(fmt, ...) \
    fprintf(stderr, "    FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)

#define ASSERT_TRUE(cond) do { \
    ctest_asserts++; \
    if (!(cond)) { \
        CTEST_FAIL_AT("ASSERT_TRUE(%s)", #cond); \
        return 1; \
    } \
} while (0)

#define ASSERT_FALSE(cond) do { \
    ctest_asserts++; \
    if (cond) { \
        CTEST_FAIL_AT("ASSERT_FALSE(%s)", #cond); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ_INT(exp, got) do { \
    long long ctest_e = (long long)(exp); \
    long long ctest_g = (long long)(got); \
    ctest_asserts++; \
    if (ctest_e != ctest_g) { \
        CTEST_FAIL_AT("%s: expected %lld, got %lld", #got, ctest_e, ctest_g); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ_U64(exp, got) do { \
    unsigned long long ctest_e = (unsigned long long)(exp); \
    unsigned long long ctest_g = (unsigned long long)(got); \
    ctest_asserts++; \
    if (ctest_e != ctest_g) { \
        CTEST_FAIL_AT("%s: expected %llu (0x%llx), got %llu (0x%llx)", \
                      #got, ctest_e, ctest_e, ctest_g, ctest_g); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ_STR(exp, got) do { \
    const char *ctest_e = (exp); \
    const char *ctest_g = (got); \
    ctest_asserts++; \
    if (ctest_e == NULL || ctest_g == NULL || strcmp(ctest_e, ctest_g) != 0) { \
        CTEST_FAIL_AT("%s: expected \"%s\", got \"%s\"", #got, \
                      ctest_e ? ctest_e : "(null)", ctest_g ? ctest_g : "(null)"); \
        return 1; \
    } \
} while (0)

#define ASSERT_MEM_EQ(exp, got, n) do { \
    const unsigned char *ctest_e = (const unsigned char *)(exp); \
    const unsigned char *ctest_g = (const unsigned char *)(got); \
    size_t ctest_n = (size_t)(n); \
    size_t ctest_i; \
    ctest_asserts++; \
    for (ctest_i = 0; ctest_i < ctest_n; ctest_i++) { \
        if (ctest_e[ctest_i] != ctest_g[ctest_i]) { \
            CTEST_FAIL_AT("memory differs at byte %zu: expected 0x%02x, got 0x%02x", \
                          ctest_i, ctest_e[ctest_i], ctest_g[ctest_i]); \
            return 1; \
        } \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    ctest_tests_run++; \
    if (fn() != 0) { \
        ctest_tests_failed++; \
        fprintf(stderr, "  FAIL %s\n", #fn); \
    } else { \
        fprintf(stderr, "  ok   %s\n", #fn); \
    } \
} while (0)

#define REWSR_TEST_MAIN(suite_fn) \
int main(void) { \
    fprintf(stderr, "%s\n", __FILE__); \
    suite_fn(); \
    fprintf(stderr, "%s: %d tests, %d failed, %d assertions\n", \
            __FILE__, ctest_tests_run, ctest_tests_failed, ctest_asserts); \
    return ctest_tests_failed != 0; \
}

#endif
