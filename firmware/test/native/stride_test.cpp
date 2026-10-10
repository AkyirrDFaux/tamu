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
    uint16_t Total = (uint16_t)(start + NaiveOffset(meta, count, count));
    std::snprintf(msg, sizeof(msg), "%s Total", label);
    CHECK_EQ(table[base + count], Total);
    // Saturating read (what LoadedScript::OffsetAt does for an out-of-range index).
    CHECK_EQ(table[base + count], Total);
}

int main()
{
    // Awkward sizes on purpose (1..8 with zeros): every alignment remainder is covered, and a
    // zero-size entry must not shift anything.
    const uint8_t InSizes[]  = {1, 5, 4, 2, 8};
    const uint8_t OutSizes[] = {3, 0, 6};
    const uint8_t VarSizes[] = {4, 1, 1, 7, 2, 3, 4};
    const uint8_t CstSizes[] = {2, 4, 1, 5};

    const uint8_t InCount = sizeof(InSizes), OutCount = sizeof(OutSizes);
    const uint8_t VarCount = sizeof(VarSizes), ConstCount = sizeof(CstSizes);

    ValueInfo in[16], out[16], var[16], cst[16];
    Fill(in, InSizes, InCount);
    Fill(out, OutSizes, OutCount);
    Fill(var, VarSizes, VarCount);
    Fill(cst, CstSizes, ConstCount);

    const uint16_t TableSize = StrideTableSize(InCount, OutCount, VarCount, ConstCount);
    CHECK_EQ(TableSize, (uint16_t)(InCount + OutCount + VarCount + ConstCount + 4));

    uint16_t table[64] = {0};
    CHECK(TableSize <= sizeof(table) / sizeof(table[0]));

    // Build exactly as LoadedScript::BuildOffsets does, through the shared base helpers.
    uint16_t k = StrideOffsetsBuild(table, StrideInBase(), in, InCount, 0);
    const uint16_t InTotal = table[InCount];
    CHECK_EQ(k, StrideOutBase(InCount));

    k = StrideOffsetsBuild(table, k, out, OutCount, InTotal);
    const uint16_t OutTotal = (uint16_t)(table[k - 1] - InTotal);
    CHECK_EQ(k, StrideVarBase(InCount, OutCount));

    k = StrideOffsetsBuild(table, k, var, VarCount, 0);
    const uint16_t VarTotal = table[k - 1];
    CHECK_EQ(k, StrideConstBase(InCount, OutCount, VarCount));

    k = StrideOffsetsBuild(table, k, cst, ConstCount, 0);
    const uint16_t ConstTotal = table[k - 1];
    CHECK_EQ(k, TableSize);

    // The four segments partition the table exactly, in the documented order.
    CHECK_EQ(StrideInBase(), 0);
    CHECK_EQ(StrideOutBase(InCount), (uint16_t)(InCount + 1));
    CHECK_EQ(StrideVarBase(InCount, OutCount), (uint16_t)(InCount + OutCount + 2));
    CHECK_EQ(StrideConstBase(InCount, OutCount, VarCount), (uint16_t)(InCount + OutCount + VarCount + 3));

    CheckSpace(table, StrideInBase(), in, InCount, 0, "input");
    CheckSpace(table, StrideOutBase(InCount), out, OutCount, InTotal, "output");
    CheckSpace(table, StrideVarBase(InCount, OutCount), var, VarCount, 0, "variable");
    CheckSpace(table, StrideConstBase(InCount, OutCount, VarCount), cst, ConstCount, 0, "constant");

    // Totals must describe the spaces the allocations are sized from.
    CHECK_EQ(InTotal, (uint16_t)(NaiveOffset(in, InCount, InCount)));
    CHECK_EQ(OutTotal, (uint16_t)(NaiveOffset(out, OutCount, OutCount)));
    CHECK_EQ(VarTotal, (uint16_t)(NaiveOffset(var, VarCount, VarCount)));
    CHECK_EQ(ConstTotal, (uint16_t)(NaiveOffset(cst, ConstCount, ConstCount)));

    // Empty spaces must still contribute their total entry and not disturb the order.
    {
        uint16_t Table[8] = {0};
        uint16_t Cursor = StrideOffsetsBuild(Table, StrideInBase(), in, 0, 0);
        CHECK_EQ(Cursor, 1);
        CHECK_EQ(Table[0], 0);
        Cursor = StrideOffsetsBuild(Table, Cursor, out, 0, 0);
        CHECK_EQ(Cursor, 2);
        CHECK_EQ(Table[1], 0);
    }

    std::printf("== stride offset tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
