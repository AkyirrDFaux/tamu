#pragma once

#include "ch32v00x.h"
#include "Core/Functions/MemoryTypes.h"
#include "Core/Types/Number.h"
#include "Core/Types/Enums.h"

// The ResistiveMeasure block (struct, schema and write triggers) is identical on the DAS
// and the Valu - it lives in Core/Devices/Measuring.h, included here after the Core types.
#include "Core/Devices/Measuring.h"

// Range selector pins (Docs/Devices.md): each channel picks a reference resistor
// between 330 Ohm / 10 kOhm / 330 kOhm. Table-driven: {port, pin} per channel x range.
struct MeasRangePin
{
    GPIO_TypeDef *Port;
    uint16_t Pin;
};
static const MeasRangePin RangePins[2][3] = {
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

static uint16_t MeasAdcRead(uint8_t Channel)
{
    ADC_RegularChannelConfig(MEAS_ADC, Channel, 1, ADC_SampleTime_73Cycles);
    ADC_SoftwareStartConvCmd(MEAS_ADC, ENABLE);
    uint32_t Start = SysTick->CNT;
    while (!ADC_GetFlagStatus(MEAS_ADC, ADC_FLAG_EOC))
    {
        if ((SysTick->CNT - Start) > ADC_EOC_TIMEOUT_CYCLES)
            break;
    }
    return ADC_GetConversionValue(MEAS_ADC);
}

// Selects the reference resistor for measurement channel `index` (0 = 330R, 1 = 10k, 2 = 330k).
static void MeasSelectRange(uint8_t Index, uint8_t Range)
{
    if (Index > 1) return;
    for (uint8_t RangeIndex = 0; RangeIndex < 3; RangeIndex++)
        PinHigh(RangePins[Index][RangeIndex].Port, RangePins[Index][RangeIndex].Pin);
    if (Range < 3)
        PinLow(RangePins[Index][Range].Port, RangePins[Index][Range].Pin);
}

// Configures the ADC and the range-selector GPIOs for both measurement channels.
void MeasuringInit()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC |
                           RCC_APB2Periph_GPIOD | RCC_APB2Periph_ADC1, ENABLE);

    // Range selectors as push-pull outputs.
    for (uint8_t Channel = 0; Channel < 2; Channel++)
        for (uint8_t RangeIndex = 0; RangeIndex < 3; RangeIndex++)
            PinModeOutput(RangePins[Channel][RangeIndex].Port, RangePins[Channel][RangeIndex].Pin);

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

    MeasSelectRange(0, 1);
    MeasSelectRange(1, 1);
}

// LDR calibration (datasheet/dsh.520-084.1.pdf): the board's part is the GL55 5-10 kOhm
// variant - light resistance at 10 lux is 5..10 kOhm, and the illuminance-resistance slope
// gamma = lg(R10/R100) is ~0.6 (Fig. 2). Calibrate both against a lux meter for the actual
// part: R10 sets the level, gamma sets the slope across the decades.
#define LDR_R10_KOHM 7.5
#define LDR_GAMMA    0.6

// log10(R10/R_ref) in Q16.16 for the {0.33, 10, 330} kOhm references of Rref_kohm, derived
// from LDR_R10_KOHM - regenerate it if that calibration changes. Folding these two constant
// logarithms out of the lux path is exactly equivalent (the same numbers to within one Q16.16
// step, a 0.006 % lux difference) and removes two log() calls and two fixed-point divisions
// from every sample.
static const int32_t KLdrLog10R10OverRref[3] = {88903, -8188, -107705};
