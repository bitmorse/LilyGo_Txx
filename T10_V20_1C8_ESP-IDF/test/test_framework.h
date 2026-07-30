// Tiny zero-dependency test framework for the host-side unit tests.
// One binary per test file (each has its own main), so file-static counters are
// fine. Use `make test` (or tools/run_tests.sh) to build and run everything.
#pragma once

#include <stdio.h>

static int tf_checks;
static int tf_fails;

#define CHECK(cond)                                                            \
    do {                                                                       \
        tf_checks++;                                                           \
        if (!(cond)) {                                                         \
            tf_fails++;                                                        \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        tf_checks++;                                                           \
        long long _a = (long long)(a), _b = (long long)(b);                    \
        if (_a != _b) {                                                        \
            tf_fails++;                                                        \
            printf("  FAIL %s:%d: %s (%lld) != %s (%lld)\n",                   \
                   __FILE__, __LINE__, #a, _a, #b, _b);                        \
        }                                                                      \
    } while (0)

#define RUN(fn)                                                                \
    do {                                                                       \
        printf("- %s\n", #fn);                                                 \
        fn();                                                                  \
    } while (0)

#define REPORT()                                                              \
    (printf("\n%s: %d checks, %d failed\n",                                    \
            tf_fails ? "FAILED" : "OK", tf_checks, tf_fails),                  \
     tf_fails ? 1 : 0)
