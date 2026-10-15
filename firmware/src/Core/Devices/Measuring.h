#pragma once

#include "Core/Functions/MemoryTypes.h"
#include "Core/Types/Number.h"
#include "Core/Types/Enums.h"

// The ResistiveMeasure block, identical on the DAS v0.1 and the Valu v2.0: the same
// documented fields, offsets, write triggers and schema
// (Docs/Modules and blocks/Measurement.md). Only the hardware side differs per device - the
// ADC setup, the reference-resistor scheme and the sample loop stay in each device's
// Measuring.h / MeasuringRun.h.

// Sensor types (Docs/Modules and blocks/Measurement.md).
enum MeasSensorType : uint8_t
{
    MeasRawMeasurement = 0,
    MeasRawVoltage = 1,
    MeasRawResistance = 2,
    MeasLDR10K = 3,
    MeasNTC10K = 4,
    MeasNTC100K = 5, // 100k nominal (R0=100k, B=3950)
};

// Resistive measurement block (Docs/Modules and blocks/Measurement.md):
//   Sampling Rate (0, P, Number), Sensor Type (1, P, Enum), Filter Coefficient
//   (2, P, Number, EMA weight of the new raw sample, 1 = no filtering), Measured Value
//   (3, RO), Current Range (4, RO).
struct ResistiveMeasPersistent
{
    Number SamplingRate = N(10);     // offset 0
    uint8_t SensorType = MeasRawMeasurement; // offset 4
    Number FilterCoeff = N(0.5);     // offset 8, EMA weight 0-1 (1 = no filtering)
};
struct ResistiveMeasVolatile
{
    Number MeasuredValue = N(0);     // offset 0
    Number CurrentRange = N(0);      // offset 4
};

// Lock the layout: the schema offsets must match the natural C struct alignment (Numbers
// are 4-aligned), so a field-reorder cannot silently desync the wire addressing again.
static_assert(offsetof(ResistiveMeasPersistent, SamplingRate) == 0, "Meas layout");
static_assert(offsetof(ResistiveMeasPersistent, SensorType) == 4, "Meas layout");
static_assert(offsetof(ResistiveMeasPersistent, FilterCoeff) == 8, "Meas layout");
static_assert(offsetof(ResistiveMeasVolatile, MeasuredValue) == 0, "Meas layout");
static_assert(offsetof(ResistiveMeasVolatile, CurrentRange) == 4, "Meas layout");

const BlockEntry ResistiveMeasEntries[] = {
    { MakeFieldKey(0, 0), 0, {(uint16_t)DataType::Number, sizeof(Number), ValueTrigger | ValuePersistent} },
    { MakeFieldKey(1, 0), 4, {(uint16_t)DataType::Enum, sizeof(uint8_t), ValuePersistent} },
    { MakeFieldKey(2, 0), 8, {(uint16_t)DataType::Number, sizeof(Number), ValueTrigger | ValuePersistent} },
    { MakeFieldKey(3, 0), 0, {(uint16_t)DataType::Number, sizeof(Number), ValueReadOnly} },
    { MakeFieldKey(4, 0), 4, {(uint16_t)DataType::Number, sizeof(Number), ValueReadOnly} },
};

// Write-time clamping for the writable Meas fields, so the STORED value always equals the
// APPLIED value (the sampling loop additionally defends in depth). Without this, an
// out-of-range write is stored verbatim while the loop silently clamps it - read-back
// would mislead.
static bool OnMeasFieldWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Len)
{
    auto *Persistent = static_cast<ResistiveMeasPersistent *>(Block.PersistentData);
    if (Len != sizeof(Number)) return false;
    Number Value = *static_cast<const Number *>(Data);

    switch (Index)
    {
    case 0: // Sampling Rate (Hz): the loop divides by it, keep >= 1 Hz and bounded
        if (Value < N(1)) Value = N(1);
        if (Value > N(1000)) Value = N(1000);
        Persistent->SamplingRate = Value;
        return true;

    case 2: // Filter Coefficient: EMA weight of the new sample, 0-1 (1 = no filtering)
        if (Value < N(0)) Value = N(0);
        if (Value > N(1)) Value = N(1);
        Persistent->FilterCoeff = Value;
        return true;
    }
    return false;
}

// Sensor Type is an enum, and the sample loop switches on it to pick the unit transform. A value
// outside the documented range would fall through the loop's default branch and publish the
// filtered raw count as whatever unit was asked for, so it is snapped to the last documented type
// here (the same "stored equals applied" rule the rate/coefficient triggers follow).
static bool OnMeasEnumWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Len)
{
    auto *Persistent = static_cast<ResistiveMeasPersistent *>(Block.PersistentData);
    if (Len != sizeof(uint8_t)) return false;
    uint8_t Value = *static_cast<const uint8_t *>(Data);
    if (Value > MeasNTC100K) Value = MeasNTC100K;
    Persistent->SensorType = Value;
    return true;
}

const BlockTrigger ResistiveMeasTriggers[] = {
    { MakeFieldKey(0, 0), OnMeasFieldWrite },
    { MakeFieldKey(1, 0), OnMeasEnumWrite },
    { MakeFieldKey(2, 0), OnMeasFieldWrite },
};

const BlockSchema ResistiveMeasSchema = {
    .Entries = ResistiveMeasEntries,
    .EntryCount = sizeof(ResistiveMeasEntries) / sizeof(BlockEntry),
    .Triggers = ResistiveMeasTriggers,
    .TriggerCount = sizeof(ResistiveMeasTriggers) / sizeof(BlockTrigger),
    .Type = BlockType::ResistiveMeasure,
};

// Clamps a raw ADC sample to the usable (1, 1022) window shared by every divider transform.
// 0 and 1023 would make the (1023 - in) denominator zero (or a tiny fraction, an unbounded
// ratio that overflows Q16.16), so both ends are pinned one step in. Used by both devices'
// sample loops (MeasuringRun.h).
static inline Number MeasClampAdc(Number In)
{
    if (In < N(1)) return N(1);
    if (In > N(1022)) return N(1022);
    return In;
}
