// The CHECK harness every native test shares.
//
// run.sh builds each test as its own binary, so the counters can live here as statics: a test
// that includes this header has exactly one pair, increments it through the macros below and
// prints it in its summary. Before this header each of the 40 tests carried its own copy of the
// two macros and the counter line (card t_6566051b), which is why jscpd saw ~900 duplicated
// lines in test/native.
#pragma once

#include <cstdio>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s = %lld, want 0x%llX\n", __FILE__, __LINE__,  \
                        #got, g, w);                                                 \
        }                                                                            \
    } while (0)
