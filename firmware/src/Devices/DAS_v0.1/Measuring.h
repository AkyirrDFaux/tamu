#pragma once

#include "ch32v00x.h"
#include "Core/Functions/MemoryTypes.h"
#include "Core/Types/Number.h"
#include "Core/Types/Enums.h"

// Resistive measurement block (Docs/Modules/Generic system blocks.md):
//   Sampling Rate (In), Filter Coefficient (In), Sensor Type (In, Enum),
//   Measured Value (Out), Current Range (Out).
struct ResistiveMeasStruct
{
    Number SamplingRate = N(10);
    Number FilterCoeff = N(0.5);
    uint8_t SensorType = 0;
    Number MeasuredValue = N(0);
    Number CurrentRange = N(0);
};

const BlockMeta ResistiveMeas_Map[] = {
    {DataType::Number | FieldFlags::Persistent, 0x00, sizeof(Number)},
    {DataType::Number | FieldFlags::Persistent, 0x00, sizeof(Number)},
    {DataType::Enum   | FieldFlags::Persistent, 0x00, sizeof(uint8_t)},
    {DataType::Number | FieldFlags::ReadOnly, 0x00, sizeof(Number)},
    {DataType::Number | FieldFlags::ReadOnly, 0x00, sizeof(Number)},
};

// Write-time clamping for the writable Meas fields, so the STORED value always equals the
// APPLIED value (the sampling loop additionally defends in depth). Without this, an
// out-of-range write is stored verbatim while the loop silently clamps it - read-back
// would mislead.
static bool OnMeasFieldWrite(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t len)
{
    auto *m = static_cast<ResistiveMeasStruct *>(block.Data);
    if (len != sizeof(Number)) return false;
    Number v = *static_cast<const Number *>(data);

    switch (index)
    {
    case 0: // Sampling Rate (Hz): the loop divides by it, keep >= 1 Hz and bounded
        if (v < N(1)) v = N(1);
        if (v > N(1000)) v = N(1000);
        m->SamplingRate = v;
        return true;

    case 1: // Filter Coefficient (EMA weight semantics: 1/(1+f)) - any f >= 0 is
            // valid, larger values average more samples
        if (v.Value < 0) v.Value = 0;
        m->FilterCoeff = v;
        return true;
    }
    return false;
}

const FieldTrigger ResistiveMeas_Triggers[] = {
    OnMeasFieldWrite,
    OnMeasFieldWrite,
    nullptr,
    nullptr,
    nullptr,
};

const uint16_t ResistiveMeas_Offsets[] = {0, 4, 8, 12, 16};

const BlockSchema ResistiveMeas_Schema = {
    .Map = ResistiveMeas_Map,
    .Triggers = ResistiveMeas_Triggers,
    .Offsets = ResistiveMeas_Offsets,
    .Type = BlockType::ResistiveMeasure,
    .MapCount = sizeof(ResistiveMeas_Map) / sizeof(BlockMeta),
};

// Range selector pins (Docs/Devices.md): each channel picks a reference resistor
// between 330 Ohm / 10 kOhm / 330 kOhm. Table-driven: {port, pin} per channel x range.
struct MeasRangePin
{
    GPIO_TypeDef *port;
    uint16_t pin;
};
static const MeasRangePin s_range_pins[2][3] = {
    // 330R              10k                330k
    {{GPIOA, GPIO_Pin_2}, {GPIOC, GPIO_Pin_7}, {GPIOD, GPIO_Pin_3}}, // channel 1
    {{GPIOC, GPIO_Pin_1}, {GPIOC, GPIO_Pin_2}, {GPIOC, GPIO_Pin_3}}, // channel 2
};

// Measuring input ADC channels (Docs/Devices.md): Measuring 1 = PD2 (A3), Measuring 2 = PC4 (A2).
#define MEAS_ADC ADC1
#define MEAS1_ADC_CH ADC_Channel_3   // PD2 (A3)
#define MEAS2_ADC_CH ADC_Channel_2   // PC4 (A2)

// Reads one 10-bit ADC sample from `channel`. Bounded by ADC_EOC_TIMEOUT_CYCLES so a
// stuck ADC cannot spin forever; returns the last conversion result on timeout.
#define ADC_EOC_TIMEOUT_CYCLES 480000UL // ~10 ms at 48 MHz

static uint16_t Meas_AdcRead(uint8_t channel)
{
    ADC_RegularChannelConfig(MEAS_ADC, channel, 1, ADC_SampleTime_73Cycles);
    ADC_SoftwareStartConvCmd(MEAS_ADC, ENABLE);
    uint32_t start = SysTick->CNT;
    while (!ADC_GetFlagStatus(MEAS_ADC, ADC_FLAG_EOC))
    {
        if ((SysTick->CNT - start) > ADC_EOC_TIMEOUT_CYCLES)
            break;
    }
    return ADC_GetConversionValue(MEAS_ADC);
}

// Selects the reference resistor for measurement channel `index` (0 = 330R, 1 = 10k, 2 = 330k).
static void Meas_SelectRange(uint8_t index, uint8_t range)
{
    if (index > 1) return;
    for (uint8_t r = 0; r < 3; r++)
        PinHigh(s_range_pins[index][r].port, s_range_pins[index][r].pin);
    if (range < 3)
        PinLow(s_range_pins[index][range].port, s_range_pins[index][range].pin);
}

// Configures the ADC and the range-selector GPIOs for both measurement channels.
void Measuring_Init()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC |
                           RCC_APB2Periph_GPIOD | RCC_APB2Periph_ADC1, ENABLE);

    // Range selectors as push-pull outputs.
    for (uint8_t c = 0; c < 2; c++)
        for (uint8_t r = 0; r < 3; r++)
            PinModeOutput(s_range_pins[c][r].port, s_range_pins[c][r].pin);

    // Measuring pins as analog inputs (PD2 and PC4).
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOD, &GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    ADC_InitTypeDef ADC_InitStructure = {0};
    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanConvMode = DISABLE;
    ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;
    ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
    ADC_InitStructure.ADC_NbrOfChannel = 1;
    ADC_Init(MEAS_ADC, &ADC_InitStructure);

    ADC_Cmd(MEAS_ADC, ENABLE);
    ADC_ResetCalibration(MEAS_ADC);
    while (ADC_GetResetCalibrationStatus(MEAS_ADC))
    {
    }
    ADC_StartCalibration(MEAS_ADC);
    while (ADC_GetCalibrationStatus(MEAS_ADC))
    {
    }

    Meas_SelectRange(0, 1);
    Meas_SelectRange(1, 1);
}

// Sensor types (Docs/Modules/Generic system blocks.md).
enum MeasSensorType : uint8_t
{
    MeasRawMeasurement = 0,
    MeasRawVoltage = 1,
    MeasRawResistance = 2,
    MeasLDR10K = 3,
    MeasNTC10K = 4,
};

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
static void Measuring_Update(uint8_t index, ResistiveMeasStruct *m, uint16_t raw)
{
    if (index > 1) return;

    // Filter weight per the Sensors.h reference: weightNew = 1/(1 + FilterCoeff).
    // Clamp FilterCoeff to >= 0 so the weight stays in (0, 1].
    Number coeff = m->FilterCoeff;
    if (coeff.Value < 0) coeff.Value = 0;
    Number weight_new = N(1) / (N(1) + coeff);

    // Auto-range from the raw sample with HYSTERESIS: each range only leaves via its
    // own threshold, so a raw sitting near a boundary cannot oscillate between two
    // references (which would re-seed the EMA filter every loop and flicker
    // CurrentRange). Range 0 = 330R, 1 = 10k, 2 = 330k reference.
    uint8_t range = s_meas_range[index];
    if (range == 0)
    {
        if (raw > 400) range = 1; // leave 330R once comfortably above
    }
    else if (range == 1)
    {
        if (raw > 850) range = 2;      // too hot for 10k -> 330k
        else if (raw < 150) range = 0; // too cold for 10k -> 330R
    }
    else
    {
        if (raw < 600) range = 1; // leave 330k once comfortably below
    }
    s_meas_range[index] = range;
    Meas_SelectRange(index, range);

    static const Number Rref_kohm[3] = {N(0.33), N(10.0), N(330.0)};
    m->CurrentRange = Rref_kohm[range];

    // Transformations operate on the RAW sample, exactly like the Sensors.h reference
    // ("SensorClass::Run"); the converted value is then EMA-filtered into MeasuredValue.
    static const Number ADCRES = N(1023);
    Number in = Number(raw);

    switch (m->SensorType)
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

    case MeasLDR10K: // lux, inverse-relation approximation for a 10k divider (Sensors.h)
    {
        if (in < N(1)) in = N(1);
        // (ADCRES - in)/in = R_ref/R_sensor. Auto-range can select the 330R or
        // 330k reference, so normalize the ratio back to the 10k reference the
        // formula assumes: * (10k / Rref_actual).
        in = N(18.0) * ((ADCRES - in) / in) * (N(10.0) / Rref_kohm[range]);
        break;
    }

    case MeasNTC10K: // degC, Steinhart-Hart simplified for a 10k divider
    {
        if (in >= ADCRES) in = N(1022);
        if (in < N(1)) in = N(1);
        // in/(ADCRES - in) = R_sensor/R_ref; normalize to R_sensor/10k.
        in = N(1) / (N(0.003354) + log((in / (ADCRES - in)) * (Rref_kohm[range] / N(10.0))) / N(3950)) - N(273.15);
        break;
    }

    default:
        break;
    }

    // EMA filter over the CONVERTED value.
    if (range != s_filt_range[index] || !s_conv_seeded[index])
        s_meas_filtered[index] = in; // re-seed after an excitation switch / first sample
    else
        s_meas_filtered[index] = (in * weight_new) +
                                 (s_meas_filtered[index] * (N(1) - weight_new));
    s_filt_range[index] = range;
    s_conv_seeded[index] = true;

    m->MeasuredValue = s_meas_filtered[index];
}
