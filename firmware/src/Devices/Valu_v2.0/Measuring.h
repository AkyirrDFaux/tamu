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
// The block schema mirrors Devices/DAS_v0.1/Measuring.h (the same documented fields and
// offsets) but the Valu's hardware differs: three channels on fixed ADC inputs (PA6/PA1/PA0)
// with NO range-selector switches (Docs/Devices.md: "Reference resistor not defined").
enum MeasSensorType : uint8_t
{
    MeasRawMeasurement = 0,
    MeasRawVoltage = 1,
    MeasRawResistance = 2,
    MeasLDR10K = 3,
    MeasNTC10K = 4,
    MeasNTC100K = 5,
};

struct ResistiveMeasPersistent
{
    Number SamplingRate = N(10);             // offset 0
    uint8_t SensorType = MeasRawMeasurement; // offset 4
    Number FilterCoeff = N(0.5);             // offset 8, EMA weight 0-1 (1 = no filtering)
};
struct ResistiveMeasVolatile
{
    Number MeasuredValue = N(0); // offset 0
    Number CurrentRange = N(0);  // offset 4
};

// Lock the layout: the schema offsets must match the natural C struct alignment (Numbers are
// 4-aligned), so a field reorder cannot silently desync the wire addressing.
static_assert(offsetof(ResistiveMeasPersistent, SamplingRate) == 0, "Meas layout");
static_assert(offsetof(ResistiveMeasPersistent, SensorType) == 4, "Meas layout");
static_assert(offsetof(ResistiveMeasPersistent, FilterCoeff) == 8, "Meas layout");
static_assert(offsetof(ResistiveMeasVolatile, MeasuredValue) == 0, "Meas layout");
static_assert(offsetof(ResistiveMeasVolatile, CurrentRange) == 4, "Meas layout");

const BlockEntry ResistiveMeas_Entries[] = {
    { MakeFieldKey(0, 0), 0, {(uint16_t)DataType::Number, sizeof(Number), ValueTrigger | ValuePersistent} },
    { MakeFieldKey(1, 0), 4, {(uint16_t)DataType::Enum, sizeof(uint8_t), ValuePersistent} },
    { MakeFieldKey(2, 0), 8, {(uint16_t)DataType::Number, sizeof(Number), ValueTrigger | ValuePersistent} },
    { MakeFieldKey(3, 0), 0, {(uint16_t)DataType::Number, sizeof(Number), ValueReadOnly} },
    { MakeFieldKey(4, 0), 4, {(uint16_t)DataType::Number, sizeof(Number), ValueReadOnly} },
};

// Write-time clamping for the writable Meas fields, so the STORED value always equals the
// APPLIED value (the sampling loop additionally defends in depth), same as the DAS.
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

    case 2: // Filter Coefficient: EMA weight of the new sample, 0-1 (1 = no filtering)
        if (v < N(0)) v = N(0);
        if (v > N(1)) v = N(1);
        m->FilterCoeff = v;
        return true;
    }
    return false;
}

const BlockTrigger ResistiveMeas_Triggers[] = {
    { MakeFieldKey(0, 0), OnMeasFieldWrite },
    { MakeFieldKey(2, 0), OnMeasFieldWrite },
};

const BlockSchema ResistiveMeas_Schema = {
    .Entries = ResistiveMeas_Entries,
    .EntryCount = sizeof(ResistiveMeas_Entries) / sizeof(BlockEntry),
    .Triggers = ResistiveMeas_Triggers,
    .TriggerCount = sizeof(ResistiveMeas_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::ResistiveMeasure,
};

// Measuring inputs (Docs/Devices.md): PA6 (ADC6), PA1 (ADC1), PA0 (ADC0).
#define MEAS_ADC ADC1
#define MEAS_CHANNEL_COUNT 3
static const uint8_t s_meas_adc_ch[MEAS_CHANNEL_COUNT] = {6, 1, 0};
static const uint16_t s_meas_pin[MEAS_CHANNEL_COUNT] = {GPIO_Pin_6, GPIO_Pin_1, GPIO_Pin_0};

// Reads one 10-bit ADC sample from `channel`. Bounded by ADC_EOC_TIMEOUT_CYCLES so a stuck
// ADC cannot spin forever; returns the last conversion result on timeout.
#define ADC_EOC_TIMEOUT_CYCLES 14400000UL // ~100 ms at 144 MHz

static uint16_t Meas_AdcRead(uint8_t channel)
{
    ADC_RegularChannelConfig(MEAS_ADC, channel, 1, ADC_SampleTime_239Cycles5);
    ADC_SoftwareStartConvCmd(MEAS_ADC, ENABLE);
    uint32_t start = SysTick->CNT;
    while (!ADC_GetFlagStatus(MEAS_ADC, ADC_FLAG_EOC))
    {
        if ((SysTick->CNT - start) > ADC_EOC_TIMEOUT_CYCLES)
            break;
    }
    return ADC_GetConversionValue(MEAS_ADC);
}

// Configures the ADC and the three measuring pins (analog inputs) for the measurement block.
void Measuring_Init()
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_ADC1, ENABLE);

    // Measuring pins as analog inputs (PA6, PA1, PA0).
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.GPIO_Pin = s_meas_pin[0] | s_meas_pin[1] | s_meas_pin[2];
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
