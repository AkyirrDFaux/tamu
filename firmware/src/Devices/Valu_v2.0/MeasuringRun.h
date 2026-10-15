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
static Number MeasFiltered[MEAS_CHANNEL_COUNT];
static bool ConvSeeded[MEAS_CHANNEL_COUNT] = {false, false, false};

// Converts a configured sampling rate (Hz) into the loop interval in ms (>= 1 ms), memoised
// per channel so the 32-bit software divide runs only when the stored rate changes.
static inline uint32_t SampleIntervalMs(uint8_t Channel)
{
    static uint32_t Cache[MEAS_CHANNEL_COUNT] = {0};
    int32_t Hz = StaticPer.Meas[Channel].SamplingRate.Value >> 16; // integer Hz
    if (Hz <= 0) Hz = 10;     // disabled / invalid -> default 10 Hz
    if (Hz > 1000) Hz = 1000; // matches the Sampling Rate write clamp
    if ((Cache[Channel] >> 16) != (uint32_t)Hz)
    {
        uint32_t Interval = 1000u / (uint32_t)Hz;
        if (Interval == 0) Interval = 1;
        Cache[Channel] = ((uint32_t)Hz << 16) | Interval;
    }
    return Cache[Channel] & 0xFFFFu;
}

// Samples one measurement channel and updates the block outputs. `index` selects the channel
// (0..2). Measured Value and Current Range are reported in kOhm so the 330 kOhm class of
// reference still fits within the Q16.16 Number range.
static void MeasuringUpdate(uint8_t Index, uint16_t Raw)
{
    if (Index >= MEAS_CHANNEL_COUNT) return;

    Number Coeff = StaticPer.Meas[Index].FilterCoeff;
    if (Coeff < N(0)) Coeff = N(0);
    if (Coeff > N(1)) Coeff = N(1);

    const Number RrefKohm = N(VALU_MEAS_REF_KOHM);
    StaticVol.Meas[Index].CurrentRange = RrefKohm;

    // EMA over the RAW ADC value (same convention as the DAS: FilterCoeff is the weight of
    // the NEW sample; 1 = no filtering, 0 = hold the previous value).
    Number FilteredRaw;
    if (!ConvSeeded[Index])
    {
        FilteredRaw = Number(Raw);
        ConvSeeded[Index] = true;
    }
    else
    {
        FilteredRaw = (Number(Raw) * Coeff) + (MeasFiltered[Index] * (N(1) - Coeff));
    }
    MeasFiltered[Index] = FilteredRaw;

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
        In = RrefKohm * In / (ADCRES - In);
        break;

    case MeasLDR10K: // lux, GL55 CdS photoresistor (gamma ~0.6, R10K calibration)
    {
        In = MeasClampAdc(In);
        const Number Ratio = In / (ADCRES - In);
        const Number Decades = (log10(N(7.5)) - log10(RrefKohm) - log10(Ratio)) / N(0.6);
        In = pow10(N(1) + Decades);
        break;
    }

    case MeasNTC10K: // degC, Steinhart-Hart simplified for a 10k divider
    case MeasNTC100K: // degC, Steinhart-Hart for a 100k nominal (R0=100k, B=3950)
    {
        In = MeasClampAdc(In);
        In = N(1) / (N(0.003354) + log((In / (ADCRES - In)) *
                   (RrefKohm / (StaticPer.Meas[Index].SensorType == MeasNTC100K ? N(100.0) : N(10.0)))) / N(3950)) - N(273.15);
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
}
