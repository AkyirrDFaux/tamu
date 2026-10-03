#pragma once

#include <cstdint>
#include "Core/Functions/MemoryTypes.h"

// Running 4-byte-aligned offsets of a ValueInfo array (the wire layout of
// Docs/Data Formats.md: each entry occupies its size rounded up to 4).
//
// Finding entry `i`'s byte offset by walking the preceding entries is O(index), and the
// script VM resolves operands per line per tick, so it keeps the prefix sums instead. They
// are built once when a script is loaded, where the layout is fixed - nothing needs
// invalidating afterwards (only values change, never the strides).
//
// Host-testable on its own (no device dependency): see test/native/stride_test.cpp.

inline uint16_t StrideAlign4(uint16_t size) { return (uint16_t)((size + 3u) & ~3u); }

// A four-space table holds one segment per space, in the fixed order inputs, outputs,
// variables, constants, each with one entry per member plus a trailing total. These give the
// segment bases (and the table size) from the counts, so the layout arithmetic lives in one
// place instead of being open-coded at each lookup.
inline uint16_t StrideInBase() { return 0; }
inline uint16_t StrideOutBase(uint8_t inCount) { return (uint16_t)(inCount + 1); }
inline uint16_t StrideVarBase(uint8_t inCount, uint8_t outCount) { return (uint16_t)(inCount + outCount + 2); }
inline uint16_t StrideConstBase(uint8_t inCount, uint8_t outCount, uint8_t varCount)
{
    return (uint16_t)(inCount + outCount + varCount + 3);
}
inline uint16_t StrideTableSize(uint8_t inCount, uint8_t outCount, uint8_t varCount, uint8_t constCount)
{
    return (uint16_t)(inCount + outCount + varCount + constCount + 4);
}

// Writes the offsets of `count` entries into `table` starting at index `base`: entry i at
// table[base + i] (relative to `start`), the space total at table[base + count] - so an index
// equal to the count is still addressable and no bounds check is needed to read the total.
// Returns the first free index after the segment.
inline uint16_t StrideOffsetsBuild(uint16_t *table, uint16_t base, const ValueInfo *meta,
                                   uint8_t count, uint16_t start)
{
    uint16_t off = start;
    for (uint8_t i = 0; i < count; i++)
    {
        table[base + i] = off;
        off = (uint16_t)(off + StrideAlign4(meta[i].Size));
    }
    table[base + count] = off;
    return (uint16_t)(base + count + 1);
}
