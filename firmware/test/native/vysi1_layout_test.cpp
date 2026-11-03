// Host-side test for the Vysi v1.0 layout-file decode (Blocks/Vysi1LayoutFile.h) - the part of the
// display's loader that decides whether a layout file is accepted and how many LEDs it implies.
//
// The point of the split: the panel's LED count follows the layout FILE, so an 86-index layout and
// an 87-index layout both load on the same binary. This test drives exactly the three calls the
// loader makes (ParseHeader, ParseTable, Count) against in-memory file images, so both cases are
// proven in one binary - and an over-capacity index is shown to be a NAMED refusal rather than the
// silent 0xFFFF the loader used to write.
//
// `Capacity` mirrors Vysi1Display::LedNum (Blocks/Vysi1Layout.h); run.sh greps that header so the
// two cannot drift apart. `MaxEntries` mirrors Vysi1Display::MaxLayoutEntries.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Blocks/Vysi1LayoutFile.h"

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

static const uint16_t Capacity = 87;  // Vysi1Display::LedNum
static const uint32_t MaxEntries = 256; // Vysi1Display::MaxLayoutEntries

// Builds a layout-file image: u8 brightness limit, u8 width, u8 height, then `CellCount` u16 LE
// cells (the format Docs/Modules and blocks/LED display.md specifies).
static uint32_t BuildFile(uint8_t *Out, uint8_t Width, uint8_t Height, const uint16_t *Cells,
                          uint32_t CellCount, uint8_t Brightness = 178)
{
    Out[0] = Brightness;
    Out[1] = Width;
    Out[2] = Height;
    for (uint32_t i = 0; i < CellCount; i++)
    {
        Out[3 + 2 * i] = (uint8_t)(Cells[i] & 0xFF);
        Out[4 + 2 * i] = (uint8_t)(Cells[i] >> 8);
    }
    return 3 + CellCount * 2;
}

// The loader's sequence, over an in-memory image: header, table, then the count the per-panel send
// walks. Nothing here is test-only logic - these are the same three calls LoadLayoutFromStorage
// makes after it has read the bytes out of storage.
static Vysi1LayoutFile::Result LoadImage(const uint8_t *File, uint32_t Size, uint16_t &OutCount)
{
    uint32_t Cells = 0;
    Vysi1LayoutFile::Result Result = Vysi1LayoutFile::ParseHeader(File, Size, MaxEntries, Cells);
    if (Result != Vysi1LayoutFile::Result::Ok)
        return Result;

    uint16_t Table[MaxEntries];
    for (uint32_t i = 0; i < Cells; i++)
        Table[i] = (uint16_t)(File[3 + 2 * i] | (File[4 + 2 * i] << 8));

    Result = Vysi1LayoutFile::ParseTable(Table, Cells, Capacity);
    if (Result != Vysi1LayoutFile::Result::Ok)
        return Result;

    OutCount = Vysi1LayoutFile::Count(Table, Cells);
    return Result;
}

int main()
{
    uint8_t File[3 + MaxEntries * 2];
    uint16_t Cells[MaxEntries];
    uint16_t Count = 0;
    uint32_t Size = 0;

    // ---- an 86-LED layout: 11x10 cells carrying LED 0..85, the rest unused ----
    for (uint32_t i = 0; i < 110; i++)
        Cells[i] = (uint16_t)(i < 86 ? i : Vysi1LayoutFile::Unused);
    Size = BuildFile(File, 11, 10, Cells, 110);
    CHECK(LoadImage(File, Size, Count) == Vysi1LayoutFile::Result::Ok);
    CHECK_EQ(Count, 86);

    // ---- an 87-LED layout: one more cell carries LED 86 (the owner's extra LED) ----
    for (uint32_t i = 0; i < 110; i++)
        Cells[i] = (uint16_t)(i < 87 ? i : Vysi1LayoutFile::Unused);
    Size = BuildFile(File, 11, 10, Cells, 110);
    CHECK(LoadImage(File, Size, Count) == Vysi1LayoutFile::Result::Ok);
    CHECK_EQ(Count, 87);

    // The count is the highest index the file carries + 1, not the cell count: a layout whose only
    // cell for LED 86 sits in the middle of a 5x2 table still sends 87 pixels.
    for (uint32_t i = 0; i < 10; i++)
        Cells[i] = Vysi1LayoutFile::Unused;
    Cells[3] = 86;
    Size = BuildFile(File, 5, 2, Cells, 10);
    CHECK(LoadImage(File, Size, Count) == Vysi1LayoutFile::Result::Ok);
    CHECK_EQ(Count, 87);

    // ... and a file that addresses nothing sends nothing (no LED is invented for it).
    for (uint32_t i = 0; i < 10; i++)
        Cells[i] = Vysi1LayoutFile::Unused;
    Size = BuildFile(File, 5, 2, Cells, 10);
    CHECK(LoadImage(File, Size, Count) == Vysi1LayoutFile::Result::Ok);
    CHECK_EQ(Count, 0);

    // ---- one LED past the chain capacity: refused BY NAME, the previous layout untouched ----
    for (uint32_t i = 0; i < 110; i++)
        Cells[i] = (uint16_t)(i < 87 ? i : Vysi1LayoutFile::Unused);
    Cells[87] = 87; // Capacity is 87, so index 87 cannot be driven
    Size = BuildFile(File, 11, 10, Cells, 110);
    Count = 0xFFFF; // must not be written on a refusal
    CHECK(LoadImage(File, Size, Count) == Vysi1LayoutFile::Result::IndexBeyondCapacity);
    CHECK_EQ(Count, 0xFFFF);
    CHECK(std::strcmp(Vysi1LayoutFile::ResultName(Vysi1LayoutFile::Result::IndexBeyondCapacity),
                      Vysi1LayoutFile::ResultName(Vysi1LayoutFile::Result::Ok)) != 0);
    CHECK(std::strstr(Vysi1LayoutFile::ResultName(Vysi1LayoutFile::Result::IndexBeyondCapacity),
                      "chain") != nullptr);

    // The largest index the capacity allows still loads (87 is the limit, 86 the highest LED).
    Cells[87] = 86;
    Size = BuildFile(File, 11, 10, Cells, 110);
    CHECK(LoadImage(File, Size, Count) == Vysi1LayoutFile::Result::Ok);
    CHECK_EQ(Count, 87);

    // ---- the layouts the akyirr_v2 setup actually ships, through the same decode ----
    // Byte-for-byte cell tables from akyirr_setup's `layouts` (semantic form: null -> Unused).
    // LAY_1 is the original 86-LED map; LAY_2 moves every picture cell by one and adds LED 0, so the
    // left panel sends 87 pixels from the same binary.
    {
        const uint16_t Unused = Vysi1LayoutFile::Unused;
        const uint16_t LAY_1[] = {
            Unused, Unused, Unused, 27, 28, 47, 48, 67, Unused, Unused, Unused,
            Unused, Unused, 10, 26, 29, 46, 49, 66, 68, Unused, Unused,
            Unused, 9, 11, 25, 30, 45, 50, 65, 69, 81, Unused,
            0, 8, 12, 24, 31, 44, 51, 64, 70, 80, 82,
            1, 7, 13, 23, 32, 43, 52, 63, 71, 79, 83,
            2, 6, 14, 22, 33, 42, 53, 62, 72, 78, 84,
            3, 5, 15, 21, 34, 41, 54, 61, 73, 77, 85,
            Unused, 4, 16, 20, 35, 40, 55, 60, 74, 76, Unused,
            Unused, Unused, 17, 19, 36, 39, 56, 59, 75, Unused, Unused,
            Unused, Unused, Unused, 18, 37, 38, 57, 58, Unused, Unused, Unused};
        const uint16_t LAY_2[] = {
            0, Unused, Unused, 28, 29, 48, 49, 68, Unused, Unused, Unused,
            Unused, Unused, 11, 27, 30, 47, 50, 67, 69, Unused, Unused,
            Unused, 10, 12, 26, 31, 46, 51, 66, 70, 82, Unused,
            1, 9, 13, 25, 32, 45, 52, 65, 71, 81, 83,
            2, 8, 14, 24, 33, 44, 53, 64, 72, 80, 84,
            3, 7, 15, 23, 34, 43, 54, 63, 73, 79, 85,
            4, 6, 16, 22, 35, 42, 55, 62, 74, 78, 86,
            Unused, 5, 17, 21, 36, 41, 56, 61, 75, 77, Unused,
            Unused, Unused, 18, 20, 37, 40, 57, 60, 76, Unused, Unused,
            Unused, Unused, Unused, 19, 38, 39, 58, 59, Unused, Unused, Unused};
        Size = BuildFile(File, 11, 10, LAY_1, 110);
        CHECK(LoadImage(File, Size, Count) == Vysi1LayoutFile::Result::Ok);
        CHECK_EQ(Count, 86);
        Size = BuildFile(File, 11, 10, LAY_2, 110);
        CHECK(LoadImage(File, Size, Count) == Vysi1LayoutFile::Result::Ok);
        CHECK_EQ(Count, 87);
    }

    // ---- a blank Layout File Name is "no layout", not "use a built-in default" ----
    // The firmware ships no layout that could be installed without a file (owner rule, 2026-11-03),
    // so the loader has to distinguish "the register names nothing" from "the file is missing": the
    // boot path turns any non-Ok result into NO map (LedCount 0, nothing rendered).
    {
        const char Blank[] = "        ";
        const char NulPadded[8] = {'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'};
        const char Mixed[8] = {' ', '\0', ' ', '\0', ' ', '\0', ' ', '\0'};
        const char Named[] = "LAY_1   ";
        CHECK(Vysi1LayoutFile::IsBlankName(Blank));
        CHECK(Vysi1LayoutFile::IsBlankName(NulPadded));
        CHECK(Vysi1LayoutFile::IsBlankName(Mixed));
        CHECK(!Vysi1LayoutFile::IsBlankName(Named));
        CHECK(std::strstr(Vysi1LayoutFile::ResultName(Vysi1LayoutFile::Result::NoName), "name") != nullptr);
    }

    // ---- malformed files ----
    uint32_t Parsed = 0;
    // Shorter than the 3-byte header.
    CHECK(Vysi1LayoutFile::ParseHeader(File, 2, MaxEntries, Parsed) ==
          Vysi1LayoutFile::Result::TooSmall);
    // Zero width or height.
    uint8_t Hdr[3] = {178, 0, 10};
    CHECK(Vysi1LayoutFile::ParseHeader(Hdr, 1000, MaxEntries, Parsed) ==
          Vysi1LayoutFile::Result::BadHeader);
    Hdr[1] = 11;
    Hdr[2] = 0;
    CHECK(Vysi1LayoutFile::ParseHeader(Hdr, 1000, MaxEntries, Parsed) ==
          Vysi1LayoutFile::Result::BadHeader);
    // More cells than the layout can hold (256): 17 x 16 = 272.
    Hdr[1] = 17;
    Hdr[2] = 16;
    CHECK(Vysi1LayoutFile::ParseHeader(Hdr, 1000, MaxEntries, Parsed) ==
          Vysi1LayoutFile::Result::BadHeader);
    // A header that promises more cells than the file carries.
    Hdr[1] = 11;
    Hdr[2] = 10;
    CHECK(Vysi1LayoutFile::ParseHeader(Hdr, 3 + 109 * 2, MaxEntries, Parsed) ==
          Vysi1LayoutFile::Result::ShortTable);
    // ... and the size that exactly covers it is accepted, with the cell count the header implies.
    CHECK(Vysi1LayoutFile::ParseHeader(Hdr, 3 + 110 * 2, MaxEntries, Parsed) ==
          Vysi1LayoutFile::Result::Ok);
    CHECK_EQ(Parsed, 110);
    // Every refusal has a name, and they are all distinct.
    const Vysi1LayoutFile::Result All[] = {
        Vysi1LayoutFile::Result::NoName, Vysi1LayoutFile::Result::Missing,
        Vysi1LayoutFile::Result::TooSmall,
        Vysi1LayoutFile::Result::BadHeader, Vysi1LayoutFile::Result::ShortTable,
        Vysi1LayoutFile::Result::ReadFailed, Vysi1LayoutFile::Result::IndexBeyondCapacity};
    for (uint32_t i = 0; i < sizeof(All) / sizeof(All[0]); i++)
    {
        CHECK(std::strcmp(Vysi1LayoutFile::ResultName(All[i]), "unknown") != 0);
        for (uint32_t j = 0; j < i; j++)
            CHECK(std::strcmp(Vysi1LayoutFile::ResultName(All[i]),
                              Vysi1LayoutFile::ResultName(All[j])) != 0);
    }

    std::printf("%s: %d checks, %d failure(s)\n", failures ? "FAILED" : "ok", checks, failures);
    return failures ? 1 : 0;
}
