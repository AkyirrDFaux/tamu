// Host-side test for the ResistiveMeasure block's two rail flags - field 5 "Open Circuit" and
// field 6 "Short Circuit" (Core/Devices/MeasuringFlags.h, wired up in each device's
// MeasuringRun.h; card t_1381f097).
//
// Both flags are a statement about the pair (range, sample): the range is one of the three
// reference resistors of the DAS's divider chain (0 = 330 Ohm, 1 = 10 kOhm, 2 = 330 kOhm) and the
// sample is the filtered raw ADC count. The contract is a full truth table, so this file walks
// every range against the interesting counts rather than checking a few hand-picked pairs:
//
//   Open  = largest range  AND sample at the UPPER limit (>= 1022)
//   Short = smallest range AND sample at the LOWER limit (<= 1)
//
// Everything else is false, including the two opposite extremes (largest range at the bottom,
// smallest range at the top) and any mid-scale sample on any range. The limits are the ends of
// the usable ADC window (1..1022, MeasClampAdc), i.e. two-count bands: `>= 1022`, not `== 1023`.
// That choice is pinned here too - a railed input dithering between 1022 and 1023 must not toggle
// the flag, which is exactly what an equality test would do.
//
// The block layout is pinned as well: fields 3 (Measured Value) and 4 (Current Range) keep their
// offsets, the two new flags land at 8 and 9, and the schema entries agree with the struct (a
// schema offset that disagrees with the struct is a silent wire-addressing bug). The header is
// host-clean - the block schema needs only MemoryTypes.h - so the real struct and the real entry
// table are what is under test, not a copy of them.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdint>
#include <cstddef>
#include <cstdio>

#include "Core/Devices/Measuring.h"

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
            std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
                        #got, #want, g, w);                                          \
        }                                                                            \
    } while (0)

// The DAS's three reference resistors, as MeasuringRun.h selects them.
static const uint8_t RangeMin = 0;    // 330 Ohm
static const uint8_t RangeMid = 1;    // 10 kOhm
static const uint8_t RangeMax = 2;    // 330 kOhm

// One row of the truth table: the whole (range, sample) -> (open, short) contract.
static void ExpectRail(const char *What, uint8_t Range, int Count, bool WantOpen, bool WantShort)
{
    const bool Open = MeasOpenCircuit(Range, N((float)Count));
    const bool Short = MeasShortCircuit(Range, N((float)Count));
    checks += 2;
    if (Open != WantOpen || Short != WantShort) {
        failures++;
        std::printf("FAIL %s: range %u, sample %d -> open %d short %d, want %d/%d\n", What,
                    (unsigned)Range, Count, (int)Open, (int)Short, (int)WantOpen, (int)WantShort);
    }
}

int main() {
    // ---- The documented truth table, row by row.
    // Largest range at the top of the window: open circuit only.
    ExpectRail("max range, full scale 1023", RangeMax, 1023, true, false);
    ExpectRail("max range, 1022 (top of the usable window)", RangeMax, 1022, true, false);
    // Smallest range at the bottom: short circuit only.
    ExpectRail("min range, zero scale 0", RangeMin, 0, false, true);
    ExpectRail("min range, 1 (bottom of the usable window)", RangeMin, 1, false, true);
    // Opposite extremes are NOT a fault each other's flag describes.
    ExpectRail("max range at the low limit", RangeMax, 0, false, false);
    ExpectRail("max range at the low limit (1)", RangeMax, 1, false, false);
    ExpectRail("min range at full scale", RangeMin, 1023, false, false);
    ExpectRail("min range at full scale (1022)", RangeMin, 1022, false, false);
    // A mid-scale sample on any range is neither rail.
    for (uint8_t Range = 0; Range < 3; Range++) {
        ExpectRail("mid range, mid scale", Range, 511, false, false);
        ExpectRail("mid range, quarter scale", Range, 256, false, false);
        ExpectRail("mid range, three-quarter scale", Range, 767, false, false);
    }
    // Just inside the limits is already NOT the limit (the bands are 2 counts wide, no wider).
    ExpectRail("max range, 1021", RangeMax, 1021, false, false);
    ExpectRail("min range, 2", RangeMin, 2, false, false);

    // ---- The full cross-product: only the two documented corners may be true, and never both.
    for (uint8_t Range = 0; Range < 3; Range++) {
        for (int Count = 0; Count <= 1023; Count++) {
            const bool Open = MeasOpenCircuit(Range, N((float)Count));
            const bool Short = MeasShortCircuit(Range, N((float)Count));
            const bool WantOpen = (Range == RangeMax) && (Count >= 1022);
            const bool WantShort = (Range == RangeMin) && (Count <= 1);
            checks++;
            if (Open != WantOpen || Short != WantShort) {
                failures++;
                std::printf("FAIL cross-product: range %u, sample %d -> open %d short %d\n",
                            (unsigned)Range, Count, (int)Open, (int)Short);
            }
            checks++;
            if (Open && Short) {
                failures++;
                std::printf("FAIL cross-product: both rails true at range %u, sample %d\n",
                            (unsigned)Range, Count);
            }
        }
    }

    // ---- Boundary / flicker: a railed input sits between 1022 and 1023 or between 0 and 1 with
    // the last bit of noise. Both counts must give the same answer, so the flag cannot chatter
    // while the reading sits at full scale - and the range it is judged against is the only thing
    // that decides which rail applies, so a sample taken before a range step answers for the range
    // it was taken on (the same rule the published Measured Value / Current Range follow).
    ExpectRail("dither 1022/1023 at the top (a)", RangeMax, 1022, true, false);
    ExpectRail("dither 1022/1023 at the top (b)", RangeMax, 1023, true, false);
    ExpectRail("dither 0/1 at the bottom (a)", RangeMin, 0, false, true);
    ExpectRail("dither 0/1 at the bottom (b)", RangeMin, 1, false, true);
    // The same railed sample on the middle range is neither rail: at 330 kOhm the auto-range has
    // just stepped UP from here (raw > 900) and at 330 Ohm it would step down, so the middle range
    // with a railed input is a transition, not a fault.
    ExpectRail("full scale while still on the middle range", RangeMid, 1023, false, false);
    ExpectRail("zero scale while still on the middle range", RangeMid, 0, false, false);
    // Fractions of a Q16.16 count cannot make a count below the limit look like the limit: 1021.99
    // is not >= 1022.
    CHECK(MeasAtUpperLimit(Number::FromRaw(N(1022).Value - 1)) == false);
    CHECK(MeasAtUpperLimit(N(1022)) == true);
    CHECK(MeasAtLowerLimit(Number::FromRaw(N(1).Value + 1)) == false);
    CHECK(MeasAtLowerLimit(N(1)) == true);
    // And a range index past the table still reads as "the largest scale" (never as neither).
    CHECK(MeasOpenCircuit(3, N(1023)) == true);

    // ---- The locked layout: fields 3/4 keep their offsets, the flags land at 8 and 9.
    CHECK_EQ(offsetof(ResistiveMeasVolatile, MeasuredValue), 0);
    CHECK_EQ(offsetof(ResistiveMeasVolatile, CurrentRange), 4);
    CHECK_EQ(offsetof(ResistiveMeasVolatile, OpenCircuit), 8);
    CHECK_EQ(offsetof(ResistiveMeasVolatile, ShortCircuit), 9);
    // 2 Numbers + 2 bytes + 2 bytes of tail padding (Number is 4-aligned). The padding is the
    // price of the locked layout: the DAS allocates two of these, so +4 B per instance.
    CHECK_EQ(sizeof(ResistiveMeasVolatile), 12);
    CHECK_EQ(alignof(ResistiveMeasVolatile), alignof(Number));

    // The schema entries and the struct must agree, byte for byte: an entry's offset is the wire
    // address, so a mismatch would read the wrong field with the right type.
    CHECK_EQ(sizeof(ResistiveMeasEntries) / sizeof(BlockEntry), 7);
    const BlockEntry *Measured = &ResistiveMeasEntries[3];
    const BlockEntry *RangeEntry = &ResistiveMeasEntries[4];
    CHECK_EQ(Measured->FieldKey, MakeFieldKey(3, 0));
    CHECK_EQ(Measured->Offset, offsetof(ResistiveMeasVolatile, MeasuredValue));
    CHECK_EQ(ValueInfoType(Measured->Info), (uint16_t)DataType::Number);
    CHECK_EQ(Measured->Info.Flags, (uint8_t)ValueReadOnly);
    CHECK_EQ(RangeEntry->FieldKey, MakeFieldKey(4, 0));
    CHECK_EQ(RangeEntry->Offset, offsetof(ResistiveMeasVolatile, CurrentRange));
    CHECK_EQ(ValueInfoType(RangeEntry->Info), (uint16_t)DataType::Number);
    CHECK_EQ(RangeEntry->Info.Flags, (uint8_t)ValueReadOnly);

    const BlockEntry *Open = &ResistiveMeasEntries[5];
    CHECK_EQ(Open->FieldKey, MakeFieldKey(5, 0));
    CHECK_EQ(Open->Offset, offsetof(ResistiveMeasVolatile, OpenCircuit));
    CHECK_EQ(Open->Offset, 8);
    CHECK_EQ(ValueInfoType(Open->Info), (uint16_t)DataType::Bool);
    CHECK_EQ(Open->Info.Size, sizeof(bool));
    CHECK_EQ(Open->Info.Flags, (uint8_t)ValueReadOnly); // RO: no trigger may ever write it

    const BlockEntry *Short = &ResistiveMeasEntries[6];
    CHECK_EQ(Short->FieldKey, MakeFieldKey(6, 0));
    CHECK_EQ(Short->Offset, offsetof(ResistiveMeasVolatile, ShortCircuit));
    CHECK_EQ(Short->Offset, 9);
    CHECK_EQ(ValueInfoType(Short->Info), (uint16_t)DataType::Bool);
    CHECK_EQ(Short->Info.Size, sizeof(bool));
    CHECK_EQ(Short->Info.Flags, (uint8_t)ValueReadOnly);

    // Both new fields are absent from the trigger table: they are outputs, not settings.
    for (size_t I = 0; I < sizeof(ResistiveMeasTriggers) / sizeof(BlockTrigger); I++) {
        CHECK(FieldOf(ResistiveMeasTriggers[I].FieldKey) != 5);
        CHECK(FieldOf(ResistiveMeasTriggers[I].FieldKey) != 6);
    }

    if (failures) {
        std::printf("%d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    std::printf("meas rail flags: %d checks passed\n", checks);
    return 0;
}
