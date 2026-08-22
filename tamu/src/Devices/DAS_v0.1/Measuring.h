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
// between 330 Ohm / 10 kOhm / 330 kOhm.
#define RNG1_LO   GPIOA, GPIO_Pin_2   // 330R
#define RNG1_MID  GPIOC, GPIO_Pin_7   // 10k
#define RNG1_HI   GPIOD, GPIO_Pin_3   // 330k
#define RNG2_LO   GPIOC, GPIO_Pin_1   // 330R
#define RNG2_MID  GPIOC, GPIO_Pin_2   // 10k
#define RNG2_HI   GPIOC, GPIO_Pin_3   // 330k

// Measuring input ADC channels (Docs/Devices.md): Measuring 1 = PD2 (A3), Measuring 2 = PC4 (A2).
#define MEAS_ADC ADC1
#define MEAS1_ADC_CH ADC_Channel_3   // PD2 (A3)
#define MEAS2_ADC_CH ADC_Channel_2   // PC4 (A2)

// Reads one 10-bit ADC sample from `channel`.
static uint16_t Meas_AdcRead(uint8_t channel)
{
    ADC_RegularChannelConfig(MEAS_ADC, channel, 1, ADC_SampleTime_73Cycles);
    ADC_SoftwareStartConvCmd(MEAS_ADC, ENABLE);
    while (!ADC_GetFlagStatus(MEAS_ADC, ADC_FLAG_EOC))
    {
    }
    return ADC_GetConversionValue(MEAS_ADC);
}

// Selects the reference resistor for measurement channel `index` (0 = 330R, 1 = 10k, 2 = 330k).
static void Meas_SelectRange(uint8_t index, uint8_t range)
{
    if (index == 0)
    {
        PinHigh(RNG1_LO); PinHigh(RNG1_MID); PinHigh(RNG1_HI);
        if (range == 0) { PinLow(RNG1_LO); PinHigh(RNG1_MID); PinHigh(RNG1_HI); }
        else if (range == 1) { PinHigh(RNG1_LO); PinLow(RNG1_MID); PinHigh(RNG1_HI); }
        else { PinHigh(RNG1_LO); PinHigh(RNG1_MID); PinLow(RNG1_HI); }
    }
    else
    {
        PinHigh(RNG2_LO); PinHigh(RNG2_MID); PinHigh(RNG2_HI);
        if (range == 0) { PinLow(RNG2_LO); PinHigh(RNG2_MID); PinHigh(RNG2_HI); }
        else if (range == 1) { PinHigh(RNG2_LO); PinLow(RNG2_MID); PinHigh(RNG2_HI); }
        else { PinHigh(RNG2_LO); PinHigh(RNG2_MID); PinLow(RNG2_HI); }
    }
}

// Configures the ADC and the range-selector GPIOs for both measurement channels.
void Measuring_Init()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC |
                           RCC_APB2Periph_GPIOD | RCC_APB2Periph_ADC1, ENABLE);

    // Range selectors as push-pull outputs.
    PinModeOutput(RNG1_LO); PinModeOutput(RNG1_MID); PinModeOutput(RNG1_HI);
    PinModeOutput(RNG2_LO); PinModeOutput(RNG2_MID); PinModeOutput(RNG2_HI);

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
// exactly the five documented fields).
static Number s_meas_filtered[2];

// Samples one measurement channel and updates the block outputs. `index` selects the
// channel (0/1). Measured Value and Current Range are reported in kOhm so that the
// 330 kOhm range still fits within the Q16.16 Number range. All arithmetic uses 32-bit
// math only (FixedMul32 / 32-bit division), so neither 64-bit multiply (__muldi3) nor
// division (__divdi3) helpers are pulled in on the flash-constrained DAS.
static void Measuring_Update(uint8_t index, ResistiveMeasStruct *m, uint16_t raw)
{
    if (index > 1) return;

    // Low-pass filter the raw 10-bit sample using FilterCoeff (0..1).
    s_meas_filtered[index] = (Number(raw) * m->FilterCoeff) +
                             (s_meas_filtered[index] * (N(1) - m->FilterCoeff));

    // Auto-range: select the reference resistor that keeps the reading mid-scale.
    uint8_t range = (raw > 850) ? 2 : ((raw < 200) ? 0 : 1);
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
