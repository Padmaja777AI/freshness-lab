/* Freshness Lab — minimal test macros (host). SPDX-License-Identifier: MIT */
#ifndef FLTEST_H
#define FLTEST_H
#include <stdio.h>
#include <string.h>

static int fl_fails = 0;
static int fl_checks = 0;

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        fl_checks++;                                                                               \
        if (!(c)) {                                                                                \
            fl_fails++;                                                                            \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                  \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        unsigned long long fl_a_ = (unsigned long long)(a);                                        \
        unsigned long long fl_b_ = (unsigned long long)(b);                                        \
        fl_checks++;                                                                               \
        if (fl_a_ != fl_b_) {                                                                      \
            fl_fails++;                                                                            \
            printf("  FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b, fl_a_,   \
                   fl_b_);                                                                         \
        }                                                                                          \
    } while (0)

#define CHECK_EQI(a, b)                                                                            \
    do {                                                                                           \
        long long fl_a_ = (long long)(a);                                                          \
        long long fl_b_ = (long long)(b);                                                          \
        fl_checks++;                                                                               \
        if (fl_a_ != fl_b_) {                                                                      \
            fl_fails++;                                                                            \
            printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, fl_a_,   \
                   fl_b_);                                                                         \
        }                                                                                          \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
    do {                                                                                           \
        double fl_a_ = (double)(a);                                                                \
        double fl_b_ = (double)(b);                                                                \
        double fl_d_ = fl_a_ > fl_b_ ? fl_a_ - fl_b_ : fl_b_ - fl_a_;                              \
        fl_checks++;                                                                               \
        if (fl_d_ > (eps)) {                                                                       \
            fl_fails++;                                                                            \
            printf("  FAIL %s:%d: %s ~ %s (%f vs %f)\n", __FILE__, __LINE__, #a, #b, fl_a_, fl_b_); \
        }                                                                                          \
    } while (0)

#define RUN(fn)                                                                                    \
    do {                                                                                           \
        int fl_before_ = fl_fails;                                                                 \
        fn();                                                                                      \
        printf("%s %s\n", fl_fails == fl_before_ ? "ok  " : "FAIL", #fn);                          \
    } while (0)

#define FLTEST_REPORT(name)                                                                        \
    (printf("%s: %d checks, %d failures\n", name, fl_checks, fl_fails), fl_fails ? 1 : 0)

#endif
