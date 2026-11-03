#pragma once

#include <cstdint>

// Decoding of a Vysi v1.0 LED layout file, split out of the display class so it can be driven by
// host tests without storage, the register bus or the render pipeline.
//
// File format (Docs/Modules and blocks/LED display.md): u8 brightness limit (0-255, read as a
// percentage), u8 width, u8 height, then width*height u16 LE cells - each a 0-based LED index, or
// 0xFFFF for a cell that drives no LED.
//
// The LED count belongs to the FILE, not to the firmware: the count is the highest index the table
// carries + 1, so an 86-index layout and an 87-index layout both load on the same binary and the
// per-panel send walks as many pixels as the file addresses. The one figure a file cannot move is
// the board's chain capacity (Vysi1Display::LedNum): a cell at or above it is a load error with a
// name, never a silent 0xFFFF.
namespace Vysi1LayoutFile
{
    // A cell that drives no LED.
    static const uint16_t Unused = 0xFFFF;

    enum class Result : uint8_t
    {
        Ok,
        Missing,             // the storage file could not be found
        TooSmall,            // shorter than the 3-byte header
        BadHeader,           // zero width or height, or more cells than the layout can hold
        ShortTable,          // shorter than 3 + width*height*2 bytes
        ReadFailed,          // storage returned fewer bytes than the file claims
        IndexBeyondCapacity, // a cell addresses an LED this board's chain cannot drive
    };

    // Human-readable form of a load result, for a log line or a diagnostic field.
    inline const char *ResultName(Result Value)
    {
        switch (Value)
        {
        case Result::Ok: return "ok";
        case Result::Missing: return "layout file not found";
        case Result::TooSmall: return "file shorter than the 3-byte header";
        case Result::BadHeader: return "bad header (zero width/height, or more cells than the layout holds)";
        case Result::ShortTable: return "file shorter than its width*height cell table";
        case Result::ReadFailed: return "storage returned fewer bytes than the file claims";
        case Result::IndexBeyondCapacity: return "a cell addresses an LED beyond this panel's chain";
        }
        return "unknown";
    }

    // Validates the 3-byte header against the file's size and the layout's entry capacity, and
    // yields the cell count (width * height) the table must carry.
    inline Result ParseHeader(const uint8_t Header[3], uint32_t FileSize, uint32_t MaxEntries,
                              uint32_t &OutCells)
    {
        if (FileSize < 3)
            return Result::TooSmall;
        const uint32_t Cells = (uint32_t)Header[1] * Header[2];
        if (Header[1] == 0 || Header[2] == 0 || Cells > MaxEntries)
            return Result::BadHeader;
        if (FileSize < 3 + Cells * 2)
            return Result::ShortTable;
        OutCells = Cells;
        return Result::Ok;
    }

    // The LED count a cell table implies: the highest index it carries, + 1 (0 when it carries
    // none). This is the whole of "the count follows the layout file".
    inline uint16_t Count(const uint16_t *Cells, uint32_t Entries)
    {
        uint32_t Highest = 0;
        bool Any = false;
        for (uint32_t i = 0; i < Entries; i++)
        {
            if (Cells[i] == Unused)
                continue;
            if (!Any || Cells[i] > Highest)
                Highest = Cells[i];
            Any = true;
        }
        return (uint16_t)(Any ? Highest + 1 : 0);
    }

    // Validates every cell against the board's chain capacity (the only firmware limit). An index at
    // or above `Capacity` rejects the whole file: the loader never rewrites it to Unused, because a
    // panel silently missing pixels is worse than a layout the operator is told about.
    inline Result ParseTable(const uint16_t *Cells, uint32_t Entries, uint16_t Capacity)
    {
        for (uint32_t i = 0; i < Entries; i++)
            if (Cells[i] != Unused && Cells[i] >= Capacity)
                return Result::IndexBeyondCapacity;
        return Result::Ok;
    }
} // namespace Vysi1LayoutFile
