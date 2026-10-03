#pragma once

// The measurement sample loop. Split from Measuring.h (which defines the block + schema) so it
// can address the board's static spaces directly, exactly like the other device implementations
// included after the block instances (see Main.h).

// Filter state for each channel (kept outside the block so the block layout stays exactly
// the five documented fields). The CONVERTED measurement is EMA-filtered; the history is
// re-seeded whenever the auto-range switches the excitation scale.
static Number s_meas_filtered[2];
static uint8_t s_meas_range[2] = {1, 1}; // currently selected range (matches Measuring_Init)
static uint8_t s_filt_range[2] = {1, 1}; // range the filter history belongs to
static bool s_conv_seeded[2] = {false, false};

// Samples one measurement channel and updates the block outputs. `index` selects the
// channel (0/1). Measured Value and Current Range are reported in kOhm so that the
// 330 kOhm range still fits within the Q16.16 Number range. All arithmetic uses 32-bit
// math only (FixedMul32 / 32-bit division), so neither 64-bit multiply (__muldi3) nor
// division (__divdi3) helpers are pulled in on the flash-constrained DAS.
static void Measuring_Update(uint8_t index, uint16_t raw)
{
    if (index > 1) return;

    // Filter weight per the docs (Docs/Modules and blocks/Measurement.md): FilterCoeff is
    // the EMA coefficient (0-1) applied on the RAW ADC value; 0 = no filtering. The raw
    // scale depends on the selected reference resistor, so the filter history is re-seeded
    // whenever the auto-range switches the excitation.
    Number coeff = staticPer.meas[index].FilterCoeff;
    if (coeff < N(0)) coeff = N(0);
    if (coeff > N(1)) coeff = N(1);

    // Auto-range from the raw sample. The divider ratio R_sensor/R_ref = raw/(1023-raw)
    // is independent of the selected reference, so the thresholds are kept in RATIO space
    // (up when the sensor is >10x the reference, down when <0.1x). This gives a wide,
    // overlap-free hysteresis band: the previous per-range raw thresholds let a mid-range
    // sensor (e.g. a 100 kOhm NTC or a dim-light LDR) flip between the 10 k and 330 k
    // references every loop, re-seeding the EMA filter and flickering CurrentRange.
    uint8_t range = s_meas_range[index];
    if (raw > 900 && range < 2) range++;        // raw/(1023-raw) > ~9 -> larger reference
    else if (raw < 93 && range > 0) range--;    // raw/(1023-raw) < 0.1 -> smaller reference
    s_meas_range[index] = range;
    Meas_SelectRange(index, range);

    static const Number Rref_kohm[3] = {N(0.33), N(10.0), N(330.0)};
    staticVol.meas[index].CurrentRange = Rref_kohm[range];

    // EMA over the RAW ADC value.
    Number filtered_raw;
    if (range != s_filt_range[index] || !s_conv_seeded[index])
    {
        filtered_raw = Number(raw); // re-seed after an excitation switch / first sample
        s_filt_range[index] = range;
    }
    else
    {
        filtered_raw = (Number(raw) * coeff) + (s_meas_filtered[index] * (N(1) - coeff));
    }
    s_meas_filtered[index] = filtered_raw;
    s_conv_seeded[index] = true;

    // Transformations operate on the FILTERED raw sample, exactly like the Sensors.h
    // reference ("SensorClass::Run").
    static const Number ADCRES = N(1023);
    Number in = filtered_raw;

    switch (staticPer.meas[index].SensorType)
    {
    case MeasRawMeasurement: // raw counts
        break;

    case MeasRawVoltage: // volts
        in = in * N(VOLTAGE) / ADCRES;
        break;

    case MeasRawResistance: // kOhm: R = Rref * V / (1 - V)
    {
        if (in >= ADCRES) in = N(1022);
        if (in < N(1)) in = N(1);
        in = Rref_kohm[range] * in / (ADCRES - in);
        break;
    }

    case MeasLDR10K: // lux from the GL55 CdS photoresistor (datasheet/dsh.520-084.1.pdf)
    {
        // Both R terms are constants, so log10(R10) - log10(R_ref) folds into the per-range
        // table below (the old form recomputed those two log10 calls - two log() calls and two
        // fixed-point divisions - on every sample). What remains is one log() call plus a
        // constant division, then the shared pow10.
        if (in < N(1)) in = N(1);
        if (in > N(1022)) in = N(1022);
        const Number ratio = in / (ADCRES - in);
        const Number log10Ratio = log(ratio) / Number::FromRaw(150902); // 1/ln 10, as log10() does
        const Number decades = (Number::FromRaw(kLdrLog10R10OverRref[range]) - log10Ratio) / N(LDR_GAMMA);
        in = pow10(N(1) + decades);
        break;
    }

    case MeasNTC10K: // degC, Steinhart-Hart simplified for a 10k divider
    case MeasNTC100K: // degC, Steinhart-Hart for a 100k nominal (R0=100k, B=3950)
    {
        if (in >= ADCRES) in = N(1022);
        if (in < N(1)) in = N(1);
        in = N(1) / (N(0.003354) + log((in / (ADCRES - in)) *
                   (Rref_kohm[range] / (staticPer.meas[index].SensorType == MeasNTC100K ? N(100.0) : N(10.0)))) / N(3950)) - N(273.15);
        break;
    }

    default:
        break;
    }

    staticVol.meas[index].MeasuredValue = in;
}
