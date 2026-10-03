#pragma once

// Fan output (Docs/Modules and blocks/Generic system blocks.md): simple PWM output on
// the specified pin.
//   Frequency (0, TR, P, uint32, Hz), Duty (1, TR, uint32, 0-100 %).
struct PWMPersistent { uint32_t PWMFreq = 25000; }; // offset 0
struct PWMVolatile   { uint32_t Duty = 0; };        // offset 0

const ValueInfo PWM_Map[] = {
    { (uint16_t)DataType::Uint32, sizeof(uint32_t), ValueTrigger | ValuePersistent },
    { (uint16_t)DataType::Uint32, sizeof(uint32_t), ValueTrigger },
};

bool OnPWMFrequencyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);
bool OnPWMDutyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);

const FieldTrigger PWM_Triggers[] = {
    OnPWMFrequencyChange,
    OnPWMDutyChange,
};

const uint16_t PWM_Offsets[] = {0, 0};

const BlockSchema PWM_Schema = {
    .Map = PWM_Map,
    .Triggers = PWM_Triggers,
    .Offsets = PWM_Offsets,
    .Type = BlockType::PWM,
    .MapCount = sizeof(PWM_Map) / sizeof(ValueInfo),
};