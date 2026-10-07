#pragma once

// The measurement sample loop for the Valu's three resistive inputs. Split from Measuring.h
// (which defines the block + schema and the ADC setup) so it can address the board's static
// spaces directly, exactly like the other device implementations included after the block
// instances (see Main.h).

// Docs/Devices.md: "Measuring PA6 (ADC6), PA1 (ADC1), PA0 (ADC0). Reference resistor not
// defined." With a single, fixed reference and no range-selector switches, there is no
// auto-ranging; this is the value the resistance/LDR/NTC transforms use. It is a placeholder
// until the hardware reference is specified (see the report/TODO) - raw/voltage measurements
// do not depend on it.
#define VALU_MEAS_REF_KOHM 10.0

// Filter state per channel (kept outside the block so the block layout stays exactly the five
// documented fields).
static Number s_meas_filtered[MEAS_CHANNEL_COUNT];
static bool s_conv_seeded[MEAS_CHANNEL_COUNT] = {false, false, false};

// Clamps a raw ADC sample to the usable (1, 1022) window shared by every divider transform.
// 0 and 1023 would make the (1023 - in) denominator zero (or a tiny fraction, an unbounded
// ratio that overflows Q16.16), so both ends are pinned one step in.
static inline Number Meas_ClampAdc(Number in)
{
    if (in < N(1)) return N(1);
    if (in > N(1022)) return N(1022);
    return in;
}

// Converts a configured sampling rate (Hz) into the loop interval in ms (>= 1 ms), memoised
// per channel so the 32-bit software divide runs only when the stored rate changes.
static inline uint32_t SampleIntervalMs(uint8_t channel)
{
    static uint32_t s_cache[MEAS_CHANNEL_COUNT] = {0};
    int32_t hz = staticPer.meas[channel].SamplingRate.Value >> 16; // integer Hz
    if (hz <= 0) hz = 10;     // disabled / invalid -> default 10 Hz
    if (hz > 1000) hz = 1000; // matches the Sampling Rate write clamp
    if ((s_cache[channel] >> 16) != (uint32_t)hz)
    {
        uint32_t interval = 1000u / (uint32_t)hz;
        if (interval == 0) interval = 1;
        s_cache[channel] = ((uint32_t)hz << 16) | interval;
    }
    return s_cache[channel] & 0xFFFFu;
}

// Samples one measurement channel and updates the block outputs. `index` selects the channel
// (0..2). Measured Value and Current Range are reported in kOhm so the 330 kOhm class of
// reference still fits within the Q16.16 Number range.
static void Measuring_Update(uint8_t index, uint16_t raw)
{
    if (index >= MEAS_CHANNEL_COUNT) return;

    Number coeff = staticPer.meas[index].FilterCoeff;
    if (coeff < N(0)) coeff = N(0);
    if (coeff > N(1)) coeff = N(1);

    const Number Rref_kohm = N(VALU_MEAS_REF_KOHM);
    staticVol.meas[index].CurrentRange = Rref_kohm;

    // EMA over the RAW ADC value (same convention as the DAS: FilterCoeff is the weight of
    // the NEW sample; 1 = no filtering, 0 = hold the previous value).
    Number filtered_raw;
    if (!s_conv_seeded[index])
    {
        filtered_raw = Number(raw);
        s_conv_seeded[index] = true;
    }
    else
    {
        filtered_raw = (Number(raw) * coeff) + (s_meas_filtered[index] * (N(1) - coeff));
    }
    s_meas_filtered[index] = filtered_raw;

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
        in = Meas_ClampAdc(in);
        in = Rref_kohm * in / (ADCRES - in);
        break;

    case MeasLDR10K: // lux, GL55 CdS photoresistor (gamma ~0.6, R10K calibration)
    {
        in = Meas_ClampAdc(in);
        const Number ratio = in / (ADCRES - in);
        const Number decades = (log10(N(7.5)) - log10(Rref_kohm) - log10(ratio)) / N(0.6);
        in = pow10(N(1) + decades);
        break;
    }

    case MeasNTC10K: // degC, Steinhart-Hart simplified for a 10k divider
    case MeasNTC100K: // degC, Steinhart-Hart for a 100k nominal (R0=100k, B=3950)
    {
        in = Meas_ClampAdc(in);
        in = N(1) / (N(0.003354) + log((in / (ADCRES - in)) *
                   (Rref_kohm / (staticPer.meas[index].SensorType == MeasNTC100K ? N(100.0) : N(10.0)))) / N(3950)) - N(273.15);
        break;
    }

    default:
        break;
    }

    staticVol.meas[index].MeasuredValue = in;
}
