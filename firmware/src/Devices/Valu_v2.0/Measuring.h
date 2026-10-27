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

    // ADC clock: BOTH prescalers on the path are at their reset values here, and neither is
    // programmed anywhere else in this env, so ADCCLK is PCLK2/2 = 144/2 = 72 MHz - 5.1x the
    // 14 MHz the part is rated for (datasheet/CH32V203.pdf electrical characteristics, "ADC clock
    // frequency fADC max = 14 MHz"; CH32VRM.pdf ch.12: "the ADCPRE[1:0] ... maximum cannot exceed
    // 14MHz", clock-tree box "ADCCLK Max=14MHz").
    //
    // The v20x ADC prescaler has only four taps - /2, /4, /6, /8 (CFGR0.ADCPRE = bits 15:14,
    // mask 0xC000; SPL ADCPrescTable {2,4,6,8}) - so the slowest tap alone still lands on 18 MHz
    // from a 144 MHz PCLK2, above the rating. Getting inside it therefore takes the APB2 prescaler
    // as well: PPRE2/2 -> PCLK2 = 72 MHz, then ADCPRE/8 -> ADCCLK = 9 MHz.
    //
    // What the PPRE2 halving does NOT cost:
    //   - TIM1 (the fan PWM, PWM.h) keeps its clock: the clock tree gives TIMxCLK for TIM1,8,9,10
    //     as 2 x PCLK2 whenever the APB2 prescaler is not 1 (CH32V203.pdf p9 and CH32VRM.pdf,
    //     "if(APB2 prescaler=1)*1 else *2"), so TIM1CLK stays 144 MHz and the period PWM.h derives
    //     from SystemCoreClock (= HCLK = 144 MHz) still matches the timer.
    //   - HCLK, SysTick and the USB clock (USBCLK from the PLL) are untouched.
    // What it does cost: APB2 register accesses (GPIOA/B, AFIO, ADC1) take 2 HCLK cycles instead
    // of 1. Nothing in this device depends on APB2 bandwidth.
    //
    // Timing at 9 MHz: the 239.5-cycle sample window becomes 26.6 us, and a whole conversion
    // (RM: TCONV = sampling + 11 ADCCLK = 250.5 cycles) 27.8 us. All three channels sampled in one
    // pass is therefore ~83 us against the 10 Hz default (1000 Hz is the Sampling Rate clamp), and
    // ADC_EOC_TIMEOUT_CYCLES below is counted in HCLK cycles, so it is unaffected.
    //
    // Rejected: /8 alone (18 MHz) - still 4 MHz over the rating; leaving the reset value (72 MHz) -
    // 5x over it. Both prescalers must be written before ADC_Init/ADC_Cmd so the ADC is never
    // clocked from an out-of-range tap.
    RCC_PCLK2Config(RCC_HCLK_Div2);
    RCC_ADCCLKConfig(RCC_PCLK2_Div8);

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
