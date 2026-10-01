/*
 * test.h - minimal host test harness (assert + counters, no framework)
 */
#ifndef TEST_HOST_TEST_H
#define TEST_HOST_TEST_H

#include <stdio.h>
#include <string.h>

static int t_checks;
static int t_failures;
static const char *t_case;

#define T_CASE(name) do { t_case = (name); printf("  case: %s\n", (name)); } while (0)

#define T_CHECK(cond) do { \
    t_checks++; \
    if (!(cond)) { \
        t_failures++; \
        printf("    FAIL %s:%d: %s (case %s)\n", __FILE__, __LINE__, #cond, t_case); \
    } \
} while (0)

#define T_CHECK_EQ_I(got, want) do { \
    long g_ = (long)(got), w_ = (long)(want); \
    t_checks++; \
    if (g_ != w_) { \
        t_failures++; \
        printf("    FAIL %s:%d: %s == %ld, want %ld (case %s)\n", \
               __FILE__, __LINE__, #got, g_, w_, t_case); \
    } \
} while (0)

#define T_RESULT(name) do { \
    printf("%s: %d checks, %d failure(s)\n", (name), t_checks, t_failures); \
    return (t_failures == 0) ? 0 : 1; \
} while (0)

#endif /* TEST_HOST_TEST_H */
