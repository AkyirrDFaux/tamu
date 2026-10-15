#pragma once

#include "ch32v20x.h"
#include "Core/Functions/MemoryTypes.h"
#include "Core/Types/Number.h"
#include "Core/Types/Enums.h"

// Resistive measurement block (Docs/Modules and blocks/Measurement.md):
//   Sampling Rate (0, P), Sensor Type (1, P, Enum), Filter Coefficient (2, P, Number, EMA
//   weight of the new raw sample, 1 = no filtering), Measured Value (3, RO), Current Range
//   (4, RO, kOhm).
//
// The block schema is shared with the DAS via Core/Devices/Measuring.h (the same documented
// fields and offsets); the Valu's hardware differs: three channels on fixed ADC inputs
// (PA6/PA1/PA0) with NO range-selector switches (Docs/Devices.md: "Reference resistor not defined").
#include "Core/Devices/Measuring.h"

// Measuring inputs (Docs/Devices.md): PA6 (ADC6), PA1 (ADC1), PA0 (ADC0).
#define MEAS_ADC ADC1
#define MEAS_CHANNEL_COUNT 3
static const uint8_t MeasAdcCh[MEAS_CHANNEL_COUNT] = {6, 1, 0};
static const uint16_t MeasPin[MEAS_CHANNEL_COUNT] = {GPIO_Pin_6, GPIO_Pin_1, GPIO_Pin_0};

// Reads one 10-bit ADC sample from `channel`. Bounded by ADC_EOC_TIMEOUT_CYCLES so a stuck
// ADC cannot spin forever; returns the last conversion result on timeout.
#define ADC_EOC_TIMEOUT_CYCLES 14400000UL // ~100 ms at 144 MHz

static uint16_t MeasAdcRead(uint8_t Channel)
{
    ADC_RegularChannelConfig(MEAS_ADC, Channel, 1, ADC_SampleTime_239Cycles5);
    ADC_SoftwareStartConvCmd(MEAS_ADC, ENABLE);
    uint32_t Start = SysTick->CNT;
    while (!ADC_GetFlagStatus(MEAS_ADC, ADC_FLAG_EOC))
    {
        if ((SysTick->CNT - Start) > ADC_EOC_TIMEOUT_CYCLES)
            break;
    }
    return ADC_GetConversionValue(MEAS_ADC);
}

// Configures the ADC and the three measuring pins (analog inputs) for the measurement block.
void MeasuringInit()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_ADC1, ENABLE);

    // Measuring pins as analog inputs (PA6, PA1, PA0).
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = MeasPin[0] | MeasPin[1] | MeasPin[2];
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

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
}
