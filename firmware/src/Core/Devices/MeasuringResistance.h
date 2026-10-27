#pragma once

#include "Core/Types/Number.h"

// The Raw Resistance unit transform (Sensor Type "Raw Resistance", field 1 of the
// ResistiveMeasure block) - R_sensor = Rref * V / (1 - V) with V = raw / 1023, i.e.
// R_kOhm = Rref_kOhm * raw / (1023 - raw).
//
// Host-clean on purpose, like MeasuringFlags.h: it is pure arithmetic on the clamped sample and
// the reference resistor, so test/native/meas_transform_test.cpp pins the whole window without
// the block schema, the storage layer or the chip header.
//
// The MULTIPLY must not run before the divide. The result is published as a Q16.16 Number whose
// ceiling is 32,767.999 kOhm, and `Rref * In` leaves that format long before the quotient does:
// on the 330 kOhm reference the product overflows from raw 100 counts up, and FixedMul32
// (Core/Types/Number.h) keeps the middle 32 bits instead of saturating, so the published value
// wraps - often into a negative number - with no flag, no log and no error. Dividing first keeps
// the intermediate in range (`raw / (1023 - raw)` <= 1022 for any usable count) and leaves only
// the genuinely unrepresentable readings to handle, which saturate here.
//
// The saturation is expressed against the product rather than as one global ratio cap: the 330
// Ohm reference can represent a ratio of 99,293x before it reaches the ceiling, and that does not
// itself fit Q16.16. So the guard is the per-reference cap below, which is exactly
// `Ratio >= ceiling / Rref`. A reference small enough that even the largest ratio (1022) cannot
// reach the ceiling skips the divide entirely: 32 kOhm * 1022 = 32,704 kOhm still fits.
static constexpr int32_t MeasResistanceCeilingKohm = 32767;

static inline Number MeasRawResistanceKohm(Number ClampedSample, Number RrefKohm)
{
    static const Number ADCRES = N(1023);

    // No usable count reaches the 1:1022 divider ratio the clamp allows, so this cannot divide by
    // zero: MeasClampAdc (Core/Devices/Measuring.h) hands over 1..1022 and the denominator stays
    // in 1..1022.
    const Number Ratio = ClampedSample / (ADCRES - ClampedSample);

    if (RrefKohm > N(32))
    {
        // 32767 / Rref is under 1024, so it fits Q16.16 and the division cannot overflow. Clamping
        // here is the documented ceiling semantics: a reading the format cannot represent is
        // published AS the ceiling (32,767 kOhm), never as a wrapped value. In practice that is
        // the 10 counts 1013..1022 on the 330 kOhm reference.
        if (Ratio >= N(MeasResistanceCeilingKohm) / RrefKohm) return N(MeasResistanceCeilingKohm);
    }

    return RrefKohm * Ratio;
}
