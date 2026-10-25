#pragma once

#include "Core/Types/Number.h"

// Open-circuit / short-circuit detection for the ResistiveMeasure block's fields 5 and 6
// (Docs/Modules and blocks/Measurement.md). Host-clean on purpose: it is pure arithmetic on the
// sample and on the range the sample came from, so test/native/meas_rail_test.cpp pins the whole
// truth table without the block schema, the storage layer or the chip header.

// The ADC's usable window is (1, 1022), not (0, 1023). MeasClampAdc (Core/Devices/Measuring.h)
// pins both ends one step in because the divider ratio raw/(1023 - raw) is unbounded (or zero)
// at 0 and 1023, so every resistance/LDR/NTC transform runs on 1..1022. The two flags test that
// same window, which makes each threshold a two-count band instead of an exact count:
//
//   upper limit: sample >= 1022   (1022 and 1023 both qualify)
//   lower limit: sample <= 1      (0 and 1 both qualify)
//
// Why not `== 1023`: a railed input dithers between 1022 and 1023 on the last bit of ADC noise,
// so an equality would make the flag flicker - exactly at the moment the value is railed and the
// flag matters. Why not a fraction of full scale (e.g. 0.9 x 1023 = 920): that is an auto-range
// threshold, not a rail - the range steps up at raw > 900 (MeasuringRun.h), so a merely-high
// reading would report an open circuit while the range is still doing its job. 1022 is both the
// transform's own top and the last count that is not the "unbounded ratio" sentinel.
//
// The range index is the selected reference resistor (MeasRangePin in each device's Measuring.h,
// Docs/Devices.md): 0 = 330 Ohm (the smallest scale), 1 = 10 kOhm, 2 = 330 kOhm (the largest).
constexpr uint8_t MeasRangeSmallest = 0;
constexpr uint8_t MeasRangeLargest = 2;

static inline bool MeasAtUpperLimit(Number Sample) { return Sample >= N(1022); }
static inline bool MeasAtLowerLimit(Number Sample) { return Sample <= N(1); }

// Open circuit: the largest reference resistor AND the input held at the top of its window, so
// the divider output can no longer rise with the sensor - nothing is loading the reference.
// `>=` rather than `==` on the range so a range index beyond the table still reads as "the
// largest scale"; the largest scale is also where the auto-range caps, so the state is stable.
static inline bool MeasOpenCircuit(uint8_t Range, Number Sample)
{
    return Range >= MeasRangeLargest && MeasAtUpperLimit(Sample);
}

// Short circuit: the smallest reference resistor AND the input held at the bottom - the sensor
// is a short across the divider. The smallest scale is the other end the auto-range caps at.
static inline bool MeasShortCircuit(uint8_t Range, Number Sample)
{
    return Range <= MeasRangeSmallest && MeasAtLowerLimit(Sample);
}
