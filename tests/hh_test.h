/*
 * Minimal assertion harness for the C test binaries.
 *
 * Deliberately dependency-free: the target is an embedded SDR build where
 * pulling in a unit-test framework is not warranted for the value it adds.
 * Each test binary returns non-zero on failure so CTest reports it.
 */
#ifndef HH_TEST_H
#define HH_TEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

static int hh_tests_run = 0;
static int hh_tests_failed = 0;
static const char *hh_current_test = "";

#define HH_FAIL(...) \
    do { \
        hh_tests_failed++; \
        fprintf(stderr, "FAIL %s (%s:%d): ", hh_current_test, __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        return; \
    } while (0)

#define HH_ASSERT(cond) \
    do { if (!(cond)) HH_FAIL("assertion failed: %s", #cond); } while (0)

#define HH_ASSERT_MSG(cond, ...) \
    do { if (!(cond)) HH_FAIL(__VA_ARGS__); } while (0)

#define HH_ASSERT_EQ_INT(a, b) \
    do { long long _a = (long long)(a), _b = (long long)(b); \
         if (_a != _b) HH_FAIL("%s == %s (%lld != %lld)", #a, #b, _a, _b); } while (0)

#define HH_ASSERT_EQ_STR(a, b) \
    do { const char *_a = (a), *_b = (b); \
         if (strcmp(_a, _b) != 0) HH_FAIL("%s == %s (\"%s\" != \"%s\")", #a, #b, _a, _b); } while (0)

#define HH_ASSERT_NEAR(a, b, eps) \
    do { double _a = (double)(a), _b = (double)(b); \
         if (fabs(_a - _b) > (eps)) HH_FAIL("%s ~= %s (%g vs %g)", #a, #b, _a, _b); } while (0)

#define HH_ASSERT_OK(expr) \
    do { hh_status_t _s = (expr); \
         if (_s != HH_OK) HH_FAIL("%s returned %s", #expr, hh_status_str(_s)); } while (0)

#define HH_ASSERT_ERR(expr, want) \
    do { hh_status_t _s = (expr); \
         if (_s != (want)) HH_FAIL("%s returned %s, want %s", #expr, hh_status_str(_s), \
                                   hh_status_str(want)); } while (0)

#define HH_RUN(fn) \
    do { \
        int _before = hh_tests_failed; \
        hh_current_test = #fn; \
        hh_tests_run++; \
        fn(); \
        if (hh_tests_failed == _before) printf("  ok   %s\n", #fn); \
    } while (0)

#define HH_TEST_MAIN_BEGIN(suite) \
    int main(void) { \
        const char *_suite = (suite); \
        printf("== %s ==\n", _suite);

#define HH_TEST_MAIN_END() \
        printf("%s: %d run, %d failed\n", _suite, hh_tests_run, hh_tests_failed); \
        return hh_tests_failed ? 1 : 0; \
    }

#endif /* HH_TEST_H */
