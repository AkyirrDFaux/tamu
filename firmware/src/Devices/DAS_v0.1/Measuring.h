#pragma once

#include "ch32v00x.h"
#include "Core/Functions/MemoryTypes.h"
#include "Core/Types/Number.h"
#include "Core/Types/Enums.h"

// Sensor types (Docs/Modules and blocks/Generic system blocks.md).
enum MeasSensorType : uint8_t
{
    MeasRawMeasurement = 0,
    MeasRawVoltage = 1,
    MeasRawResistance = 2,
    MeasLDR10K = 3,
    MeasNTC10K = 4,
    MeasNTC100K = 5, // 100k nominal (R0=100k, B=3950)
};

// Resistive measurement block (Docs/Modules and blocks/Measurement.md):
//   Sampling Rate (0, P, Number), Sensor Type (1, P, Enum), Filter Coefficient
//   (2, P, Number, EMA 0-1 on the raw ADC), Measured Value (3, RO), Current Range (4, RO).
struct ResistiveMeasPersistent
{
    Number SamplingRate = N(10);     // offset 0
    uint8_t SensorType = MeasRawMeasurement; // offset 4
    Number FilterCoeff = N(0.5);     // offset 8, EMA weight 0-1
};
struct ResistiveMeasVolatile
{
    Number MeasuredValue = N(0);     // offset 0
    Number CurrentRange = N(0);      // offset 4
};

// Lock the layout: the schema offsets must match the natural C struct alignment (Numbers
// are 4-aligned), so a field-reorder cannot silently desync the wire addressing again.
static_assert(offsetof(ResistiveMeasPersistent, SamplingRate) == 0, "Meas layout");
static_assert(offsetof(ResistiveMeasPersistent, SensorType) == 4, "Meas layout");
static_assert(offsetof(ResistiveMeasPersistent, FilterCoeff) == 8, "Meas layout");
static_assert(offsetof(ResistiveMeasVolatile, MeasuredValue) == 0, "Meas layout");
static_assert(offsetof(ResistiveMeasVolatile, CurrentRange) == 4, "Meas layout");



const ValueInfo ResistiveMeas_Map[] = {
    {(uint16_t)DataType::Number, sizeof(Number), ValuePersistent},
    {(uint16_t)DataType::Enum, sizeof(uint8_t), ValuePersistent},
    {(uint16_t)DataType::Number, sizeof(Number), ValuePersistent},
    {(uint16_t)DataType::Number, sizeof(Number), ValueReadOnly},
    {(uint16_t)DataType::Number, sizeof(Number), ValueReadOnly},
};

// Write-time clamping for the writable Meas fields, so the STORED value always equals the
// APPLIED value (the sampling loop additionally defends in depth). Without this, an
// out-of-range write is stored verbatim while the loop silently clamps it - read-back
// would mislead.
static bool OnMeasFieldWrite(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t len)
{
    auto *m = static_cast<ResistiveMeasPersistent *>(block.PersistentData);
    if (len != sizeof(Number)) return false;
    Number v = *static_cast<const Number *>(data);

    switch (index)
    {
    case 0: // Sampling Rate (Hz): the loop divides by it, keep >= 1 Hz and bounded
        if (v < N(1)) v = N(1);
        if (v > N(1000)) v = N(1000);
        m->SamplingRate = v;
        return true;

    case 2: // Filter Coefficient: EMA weight 0-1 (0 = no filtering)
        if (v < N(0)) v = N(0);
        if (v > N(1)) v = N(1);
        m->FilterCoeff = v;
        return true;
    }
    return false;
}

const FieldTrigger ResistiveMeas_Triggers[] = {
    OnMeasFieldWrite,
    nullptr,
    OnMeasFieldWrite,
    nullptr,
    nullptr,
};

const uint16_t ResistiveMeas_Offsets[] = {0, 4, 8, 0, 4};

const BlockSchema ResistiveMeas_Schema = {
    .Map = ResistiveMeas_Map,
    .Triggers = ResistiveMeas_Triggers,
    .Offsets = ResistiveMeas_Offsets,
    .Type = BlockType::ResistiveMeasure,
    .MapCount = sizeof(ResistiveMeas_Map) / sizeof(ValueInfo),
    .VolatileSize = 8,
    .PersistentSize = 12,
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
static const int32_t kLdrLog10R10OverRref[3] = {88903, -8188, -107705};
