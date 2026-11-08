#ifndef BTA_TEST_H
#define BTA_TEST_H
/* Minimal host test harness: no framework, just counters and a CHECK macro.
 * Each module has a test_<module>() entry point; main.c runs them all and
 * returns non-zero if any check failed. */
#include <stdio.h>

struct test_counts { int checks, failures; };
extern struct test_counts g_test;

#define CHECK(cond) do { \
    g_test.checks++; \
    if (!(cond)) { \
        g_test.failures++; \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* Compare two long values and print both on mismatch (handy for parsers/maths). */
#define CHECK_EQ(got, want) do { \
    g_test.checks++; \
    long _g = (long)(got), _w = (long)(want); \
    if (_g != _w) { \
        g_test.failures++; \
        printf("  FAIL %s:%d  %s == %s  (got %ld, want %ld)\n", __FILE__, __LINE__, #got, #want, _g, _w); \
    } \
} while (0)

void test_json(void);
void test_model(void);
void test_auth(void);
void test_strfmt(void);
void test_dsp(void);
void test_conn(void);
void test_profiles(void);
void test_timing(void);
void test_diag_v6(void);
void test_pacing(void);
void test_qr(void);
void test_kernel(void);
void test_v13_sender(void);
#endif
