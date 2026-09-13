/*
 * ctest.h, minimal test harness for the cprobe C tree.
 *
 * Deliberately header-only and dependency-free so a test file is just
 * one .c that includes this and links against librewsrprobe.a. Tests
 * are plain void(void) functions. The ASSERT_* macros record a failure
 * and return from the current test function, so a test stops at its
 * first broken assertion instead of cascading noise.
 *
 * Usage:
 *   static void test_something(void) { ASSERT_EQ_INT(1, f()); }
 *   REWSR_TEST_MAIN(
 *       RUN_TEST(test_something);
 *   )
 *
 * The generated main returns nonzero when any test failed, which is
 * what the Makefile keys off.
 */
#ifndef REWSR_CTEST_H
#define REWSR_CTEST_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int rewsr_test_total = 0;
static int rewsr_test_failed = 0;
/* Set by an ASSERT inside the currently running test, cleared by RUN_TEST. */
static int rewsr_test_cur_failed = 0;

#define RUN_TEST(fn)                                                        \
    do {                                                                    \
        rewsr_test_cur_failed = 0;                                          \
        rewsr_test_total++;                                                 \
        fn();                                                               \
        if (rewsr_test_cur_failed) {                                        \
            rewsr_test_failed++;                                            \
            fprintf(stderr, "FAIL %s\n", #fn);                              \
        } else {                                                            \
            fprintf(stderr, "ok   %s\n", #fn);                              \
        }                                                                   \
    } while (0)

#define REWSR_TEST_FAILURE_PREAMBLE()                                       \
    do {                                                                    \
        rewsr_test_cur_failed = 1;                                          \
        fprintf(stderr, "  assert failed %s:%d: ", __FILE__, __LINE__);     \
    } while (0)

#define ASSERT_TRUE(expr)                                                   \
    do {                                                                    \
        if (!(expr)) {                                                      \
            REWSR_TEST_FAILURE_PREAMBLE();                                  \
            fprintf(stderr, "ASSERT_TRUE(%s)\n", #expr);                    \
            return;                                                         \
        }                                                                   \
    } while (0)

#define ASSERT_EQ_INT(expected, actual)                                     \
    do {                                                                    \
        long long rewsr_e_ = (long long)(expected);                         \
        long long rewsr_a_ = (long long)(actual);                           \
        if (rewsr_e_ != rewsr_a_) {                                         \
            REWSR_TEST_FAILURE_PREAMBLE();                                  \
            fprintf(stderr, "%s: expected %lld, got %lld\n",                \
                    #actual, rewsr_e_, rewsr_a_);                           \
            return;                                                         \
        }                                                                   \
    } while (0)

#define ASSERT_EQ_U64(expected, actual)                                     \
    do {                                                                    \
        uint64_t rewsr_e_ = (uint64_t)(expected);                           \
        uint64_t rewsr_a_ = (uint64_t)(actual);                             \
        if (rewsr_e_ != rewsr_a_) {                                         \
            REWSR_TEST_FAILURE_PREAMBLE();                                  \
            fprintf(stderr,                                                 \
                    "%s: expected %" PRIu64 " (0x%" PRIx64 "), "            \
                    "got %" PRIu64 " (0x%" PRIx64 ")\n",                    \
                    #actual, rewsr_e_, rewsr_e_, rewsr_a_, rewsr_a_);       \
            return;                                                         \
        }                                                                   \
    } while (0)

#define ASSERT_EQ_STR(expected, actual)                                     \
    do {                                                                    \
        const char *rewsr_e_ = (expected);                                  \
        const char *rewsr_a_ = (actual);                                    \
        if (rewsr_e_ == NULL || rewsr_a_ == NULL ||                         \
            strcmp(rewsr_e_, rewsr_a_) != 0) {                              \
            REWSR_TEST_FAILURE_PREAMBLE();                                  \
            fprintf(stderr, "%s: expected \"%s\", got \"%s\"\n", #actual,   \
                    rewsr_e_ ? rewsr_e_ : "(null)",                         \
                    rewsr_a_ ? rewsr_a_ : "(null)");                        \
            return;                                                         \
        }                                                                   \
    } while (0)

#define ASSERT_MEM_EQ(expected, actual, nbytes)                             \
    do {                                                                    \
        const void *rewsr_e_ = (expected);                                  \
        const void *rewsr_a_ = (actual);                                    \
        size_t rewsr_n_ = (size_t)(nbytes);                                 \
        if (memcmp(rewsr_e_, rewsr_a_, rewsr_n_) != 0) {                    \
            size_t rewsr_i_;                                                \
            const unsigned char *rewsr_eb_ =                                \
                (const unsigned char *)rewsr_e_;                            \
            const unsigned char *rewsr_ab_ =                                \
                (const unsigned char *)rewsr_a_;                            \
            REWSR_TEST_FAILURE_PREAMBLE();                                  \
            fprintf(stderr, "ASSERT_MEM_EQ(%s, %s, %zu), first diff at ",   \
                    #expected, #actual, rewsr_n_);                          \
            for (rewsr_i_ = 0; rewsr_i_ < rewsr_n_; rewsr_i_++) {           \
                if (rewsr_eb_[rewsr_i_] != rewsr_ab_[rewsr_i_]) {           \
                    fprintf(stderr,                                         \
                            "offset %zu (expected 0x%02x, got 0x%02x)\n",   \
                            rewsr_i_, rewsr_eb_[rewsr_i_],                  \
                            rewsr_ab_[rewsr_i_]);                           \
                    break;                                                  \
                }                                                           \
            }                                                               \
            return;                                                         \
        }                                                                   \
    } while (0)

static inline int rewsr_test_finish(void) {
    fprintf(stderr, "%d tests, %d failed\n",
            rewsr_test_total, rewsr_test_failed);
    return rewsr_test_failed != 0;
}

#define REWSR_TEST_MAIN(...)                                                \
    int main(void) {                                                        \
        __VA_ARGS__                                                         \
        return rewsr_test_finish();                                         \
    }

#endif /* REWSR_CTEST_H */
