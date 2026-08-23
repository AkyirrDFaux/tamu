#pragma once

#include "ch32v00x.h"
#include "Core/Functions/SystemMemory.h"
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
    {DataType::Number | FieldFlags::None,     0x00, sizeof(Number)},
    {DataType::Number | FieldFlags::None,     0x00, sizeof(Number)},
    {DataType::Enum   | FieldFlags::None,     0x00, sizeof(uint8_t)},
    {DataType::Number | FieldFlags::ReadOnly, 0x00, sizeof(Number)},
    {DataType::Number | FieldFlags::ReadOnly, 0x00, sizeof(Number)},
};

const BlockSchema ResistiveMeas_Schema = {
    .Map = ResistiveMeas_Map,
    .Triggers = nullptr,
    .Type = BlockType::ResistiveMeasure,
    .MapCount = sizeof(ResistiveMeas_Map) / sizeof(BlockMeta),
    .TriggerCount = 0,
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

// Low-pass filter state for each channel (kept outside the block so the block layout stays
// exactly the five documented fields), plus the range each filter is valid for: switching
// the reference resistor changes the excitation scale, so the filter history must be
// re-seeded on every range transition.
static Number s_meas_filtered[2];
static uint8_t s_meas_range[2] = {1, 1}; // matches Measuring_Init's mid-range selection
static bool s_meas_seeded[2] = {false, false};

// Samples one measurement channel and updates the block outputs. `index` selects the
// channel (0/1). Measured Value and Current Range are reported in kOhm so that the
// 330 kOhm range still fits within the Q16.16 Number range. All arithmetic uses 32-bit
// math only (FixedMul32 / 32-bit division), so neither 64-bit multiply (__muldi3) nor
// division (__divdi3) helpers are pulled in on the flash-constrained DAS.
static void Measuring_Update(uint8_t index, ResistiveMeasStruct *m, uint16_t raw)
{
    if (index > 1) return;

    // Clamp FilterCoeff to [0,1]: a coefficient outside this range makes the
    // low-pass diverge instead of converge.
    Number coeff = m->FilterCoeff;
    if (coeff.Value < 0) coeff.Value = 0;
    if (coeff.Value > (1 << 16)) coeff.Value = (1 << 16);

    // Auto-range from the *filtered* value so noisy inputs near a threshold do not
    // chatter between ranges. On a range switch the old history belongs to another
    // excitation scale, so re-seed the filter with the current raw sample.
    bool seeded = s_meas_seeded[index];
    if (!seeded)
    {
        s_meas_filtered[index] = Number(raw);
        s_meas_seeded[index] = true;
    }
    else
    {
        s_meas_filtered[index] = (Number(raw) * coeff) +
                                 (s_meas_filtered[index] * (N(1) - coeff));
    }

    uint16_t filtered = (uint16_t)(s_meas_filtered[index].Value >> 16);
    uint8_t range = (filtered > 850) ? 2 : ((filtered < 200) ? 0 : 1);
    if (range != s_meas_range[index])
    {
        s_meas_range[index] = range;
        s_meas_filtered[index] = Number(raw); // discard stale-scale history
    }
    Meas_SelectRange(index, range);

    const Number Rref_kohm[3] = {N(0.33), N(10.0), N(330.0)};
    m->CurrentRange = Rref_kohm[range];

    // Report the value according to the selected sensor type.
    switch (m->SensorType)
    {
    case MeasRawMeasurement: // filtered ADC sample (0..1023)
        m->MeasuredValue = s_meas_filtered[index];
        break;
    case MeasRawVoltage: // filtered sample as volts (0..3.3), 32-bit math only
        m->MeasuredValue = Number::FromRaw(
            FixedMul32(s_meas_filtered[index].Value / 1023, N(VOLTAGE).Value));
        break;
    default: // Raw Resistance, LDR 10K, NTC 10K -> resistance in kOhm
    {
        // Resistance (kOhm) = Rref * V / (1 - V).
        int32_t vi = s_meas_filtered[index].Value >> 16; // integer part of the filtered sample (0..1024)
        if (vi > 1000) vi = 1000;                        // keep the divider from collapsing
        if (vi < 1) vi = 1;
        int32_t denom = 1024 - vi;
        int32_t ratio = ((int32_t)(vi << 16)) / denom;   // Q16.16 ratio, 32-bit division
        m->MeasuredValue.Value = FixedMul32(Rref_kohm[range].Value, ratio);
        break;
    }
    }
}
