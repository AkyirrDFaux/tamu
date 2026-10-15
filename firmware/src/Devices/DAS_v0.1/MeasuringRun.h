#pragma once

// The measurement sample loop. Split from Measuring.h (which defines the block + schema) so it
// can address the board's static spaces directly, exactly like the other device implementations
// included after the block instances (see Main.h).

// Filter state for each channel (kept outside the block so the block layout stays exactly
// the five documented fields). The RAW ADC sample is EMA-filtered; the history is re-seeded
// whenever the auto-range switches the excitation scale.
static Number MeasFiltered[2];
static uint8_t MeasRange[2] = {1, 1}; // currently selected range (matches Measuring_Init)
static uint8_t FiltRange[2] = {1, 1}; // range the filter history belongs to
static bool ConvSeeded[2] = {false, false};

// Samples one measurement channel and updates the block outputs. `index` selects the
// channel (0/1). Measured Value and Current Range are reported in kOhm so that the
// 330 kOhm range still fits within the Q16.16 Number range. All arithmetic uses 32-bit
// math only (FixedMul32 / 32-bit division), so neither 64-bit multiply (__muldi3) nor
// division (__divdi3) helpers are pulled in on the flash-constrained DAS.
static void MeasuringUpdate(uint8_t Index, uint16_t Raw)
{
    if (Index > 1) return;

    // Filter weight per the docs (Docs/Modules and blocks/Measurement.md): FilterCoeff is
    // the EMA weight of the NEW raw sample (0-1). 1 = no filtering (the output tracks the
    // raw value); 0 = hold the previous sample (maximum smoothing) - the same convention as
    // AccGyr. The raw scale depends on the selected reference resistor, so the filter
    // history is re-seeded whenever the auto-range switches the excitation.
    Number Coeff = StaticPer.Meas[Index].FilterCoeff;
    if (Coeff < N(0)) Coeff = N(0);
    if (Coeff > N(1)) Coeff = N(1);

    // The reference that was ACTIVE when `raw` was sampled. The sample and every transform
    // below belong to this range; the new range decided at the end only takes effect for the
    // NEXT sample. (Previously the reference was switched before the transform, so an ADC
    // sample taken on the old reference was converted with the new Rref on every range
    // transition and mis-seeded the EMA.)
    uint8_t Range = MeasRange[Index];
    static const Number RrefKohm[3] = {N(0.33), N(10.0), N(330.0)};
    StaticVol.Meas[Index].CurrentRange = RrefKohm[Range];

    // EMA over the RAW ADC value. The history is in the active range's ADC units, so it is
    // re-seeded when that range differs from the one the history belongs to.
    Number FilteredRaw;
    if (Range != FiltRange[Index] || !ConvSeeded[Index])
    {
        FilteredRaw = Number(Raw); // re-seed after an excitation switch / first sample
        FiltRange[Index] = Range;
    }
    else
    {
        FilteredRaw = (Number(Raw) * Coeff) + (MeasFiltered[Index] * (N(1) - Coeff));
    }
    MeasFiltered[Index] = FilteredRaw;
    ConvSeeded[Index] = true;

    // Transformations operate on the FILTERED raw sample.
    static const Number ADCRES = N(1023);
    Number In = FilteredRaw;

    switch (StaticPer.Meas[Index].SensorType)
    {
    case MeasRawMeasurement: // raw counts
        break;

    case MeasRawVoltage: // volts
        In = In * N(VOLTAGE) / ADCRES;
        break;

    case MeasRawResistance: // kOhm: R = Rref * V / (1 - V)
        In = MeasClampAdc(In);
        In = RrefKohm[Range] * In / (ADCRES - In);
        break;

    case MeasLDR10K: // lux from the GL55 CdS photoresistor (datasheet/dsh.520-084.1.pdf)
    {
        // Both R terms are constants, so log10(R10) - log10(R_ref) folds into the per-range
        // table below (the old form recomputed those two log10 calls - two log() calls and two
        // fixed-point divisions - on every sample). What remains is one log10() call (the shared
        // Number.h helper) plus the per-range constant division, then the shared pow10.
        In = MeasClampAdc(In);
        const Number Ratio = In / (ADCRES - In);
        const Number Log10Ratio = log10(Ratio);
        const Number Decades = (Number::FromRaw(KLdrLog10R10OverRref[Range]) - Log10Ratio) / N(LDR_GAMMA);
        In = pow10(N(1) + Decades);
        break;
    }

    case MeasNTC10K: // degC, Steinhart-Hart simplified for a 10k divider
    case MeasNTC100K: // degC, Steinhart-Hart for a 100k nominal (R0=100k, B=3950)
    {
        In = MeasClampAdc(In);
        In = N(1) / (N(0.003354) + log((In / (ADCRES - In)) *
                   (RrefKohm[Range] / (StaticPer.Meas[Index].SensorType == MeasNTC100K ? N(100.0) : N(10.0)))) / N(3950)) - N(273.15);
        break;
    }

    default:
        // A Sensor Type outside the enum has no unit transform; report 0 rather than silently
        // passing the filtered raw count off as the requested unit (the write trigger snaps such
        // a value, so this is defence in depth).
        In = N(0);
        break;
    }

    StaticVol.Meas[Index].MeasuredValue = In;

    // Auto-range from the raw sample, applied AFTER the transform used the active range. The
    // divider ratio R_sensor/R_ref = raw/(1023-raw) is independent of the selected reference,
    // so the thresholds are kept in RATIO space (up when the sensor is > ~7.3x the reference,
    // down when < 0.1x). This gives a wide, overlap-free hysteresis band: the previous
    // per-range raw thresholds let a mid-range sensor (e.g. a 100 kOhm NTC or a dim-light LDR)
    // flip between the 10 k and 330 k references every loop, re-seeding the EMA filter and
    // flickering CurrentRange. The range pins are only driven when the range actually changes.
    static const uint16_t RANGE_UP_RAW = 900;  // raw/(1023-raw) > ~7.3 -> larger reference
    static const uint16_t RANGE_DOWN_RAW = 93; // raw/(1023-raw) < 0.1  -> smaller reference
    uint8_t NewRange = Range;
    if (Raw > RANGE_UP_RAW && NewRange < 2) NewRange++;
    else if (Raw < RANGE_DOWN_RAW && NewRange > 0) NewRange--;
    if (NewRange != Range)
    {
        MeasRange[Index] = NewRange;
        MeasSelectRange(Index, NewRange);
    }
}
