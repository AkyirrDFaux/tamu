// Host-side test for Core/Functions/StrideOffsets.h - the prefix-sum offset tables the script
// VM uses to resolve a symbol's byte offset in O(1).
//
// The property that matters is that the table entry for symbol `i` equals the offset the old
// per-lookup walk produced (the sum of the preceding 4-byte-aligned strides), and that the
// four spaces land in their own segments of the shared table. The segment bases come from the
// same inline helpers the firmware's getters use, so a mistake there fails here too.
//
// No device dependency: build with -I firmware/src and a plain host compiler.

#include <cstdint>
#include <cstdio>

#include "Core/Functions/StrideOffsets.h"

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
            std::printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__,    \
                        #got, g, w);                                                 \
        }                                                                            \
    } while (0)

// The reference the tables replaced: walk everything before `i`.
static uint16_t NaiveOffset(const ValueInfo *meta, uint8_t count, uint8_t i)
{
    uint16_t off = 0;
    for (uint8_t k = 0; k < i && k < count; k++) off += StrideAlign4(meta[k].Size);
    return off;
}

static void Fill(ValueInfo *meta, const uint8_t *sizes, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        meta[i].Type = 0;
        meta[i].Flags = 0;
        meta[i].Size = sizes[i];
    }
}

// Every entry of one space, plus the total, plus one index past the end (which must saturate).
static void CheckSpace(const uint16_t *table, uint16_t base, const ValueInfo *meta,
                       uint8_t count, uint16_t start, const char *label)
{
    char msg[64];
    for (uint8_t i = 0; i < count; i++)
    {
        std::snprintf(msg, sizeof(msg), "%s offset %u", label, (unsigned)i);
        CHECK_EQ(table[base + i], (uint16_t)(start + NaiveOffset(meta, count, i)));
    }
    uint16_t total = (uint16_t)(start + NaiveOffset(meta, count, count));
    std::snprintf(msg, sizeof(msg), "%s total", label);
    CHECK_EQ(table[base + count], total);
    // Saturating read (what LoadedScript::OffsetAt does for an out-of-range index).
    CHECK_EQ(table[base + count], total);
}

int main()
{
    // Awkward sizes on purpose (1..8 with zeros): every alignment remainder is covered, and a
    // zero-size entry must not shift anything.
    const uint8_t inSizes[]  = {1, 5, 4, 2, 8};
    const uint8_t outSizes[] = {3, 0, 6};
    const uint8_t varSizes[] = {4, 1, 1, 7, 2, 3, 4};
    const uint8_t cstSizes[] = {2, 4, 1, 5};

    const uint8_t inCount = sizeof(inSizes), outCount = sizeof(outSizes);
    const uint8_t varCount = sizeof(varSizes), constCount = sizeof(cstSizes);

    ValueInfo in[16], out[16], var[16], cst[16];
    Fill(in, inSizes, inCount);
    Fill(out, outSizes, outCount);
    Fill(var, varSizes, varCount);
    Fill(cst, cstSizes, constCount);

    const uint16_t n = StrideTableSize(inCount, outCount, varCount, constCount);
    CHECK_EQ(n, (uint16_t)(inCount + outCount + varCount + constCount + 4));

    uint16_t table[64] = {0};
    CHECK(n <= sizeof(table) / sizeof(table[0]));

    // Build exactly as LoadedScript::BuildOffsets does, through the shared base helpers.
    uint16_t k = StrideOffsetsBuild(table, StrideInBase(), in, inCount, 0);
    const uint16_t inTotal = table[inCount];
    CHECK_EQ(k, StrideOutBase(inCount));

    k = StrideOffsetsBuild(table, k, out, outCount, inTotal);
    const uint16_t outTotal = (uint16_t)(table[k - 1] - inTotal);
    CHECK_EQ(k, StrideVarBase(inCount, outCount));

    k = StrideOffsetsBuild(table, k, var, varCount, 0);
    const uint16_t varTotal = table[k - 1];
    CHECK_EQ(k, StrideConstBase(inCount, outCount, varCount));

    k = StrideOffsetsBuild(table, k, cst, constCount, 0);
    const uint16_t constTotal = table[k - 1];
    CHECK_EQ(k, n);

    // The four segments partition the table exactly, in the documented order.
    CHECK_EQ(StrideInBase(), 0);
    CHECK_EQ(StrideOutBase(inCount), (uint16_t)(inCount + 1));
    CHECK_EQ(StrideVarBase(inCount, outCount), (uint16_t)(inCount + outCount + 2));
    CHECK_EQ(StrideConstBase(inCount, outCount, varCount), (uint16_t)(inCount + outCount + varCount + 3));

    CheckSpace(table, StrideInBase(), in, inCount, 0, "input");
    CheckSpace(table, StrideOutBase(inCount), out, outCount, inTotal, "output");
    CheckSpace(table, StrideVarBase(inCount, outCount), var, varCount, 0, "variable");
    CheckSpace(table, StrideConstBase(inCount, outCount, varCount), cst, constCount, 0, "constant");

    // Totals must describe the spaces the allocations are sized from.
    CHECK_EQ(inTotal, (uint16_t)(NaiveOffset(in, inCount, inCount)));
    CHECK_EQ(outTotal, (uint16_t)(NaiveOffset(out, outCount, outCount)));
    CHECK_EQ(varTotal, (uint16_t)(NaiveOffset(var, varCount, varCount)));
    CHECK_EQ(constTotal, (uint16_t)(NaiveOffset(cst, constCount, constCount)));

    // Empty spaces must still contribute their total entry and not disturb the order.
    {
        uint16_t t[8] = {0};
        uint16_t j = StrideOffsetsBuild(t, StrideInBase(), in, 0, 0);
        CHECK_EQ(j, 1);
        CHECK_EQ(t[0], 0);
        j = StrideOffsetsBuild(t, j, out, 0, 0);
        CHECK_EQ(j, 2);
        CHECK_EQ(t[1], 0);
    }

    std::printf("== stride offset tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
