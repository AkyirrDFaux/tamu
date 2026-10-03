#pragma once

// Fan output (Docs/Modules and blocks/Generic system blocks.md): simple PWM output on
// the specified pin.
//   Frequency (0, TR, P, uint32, Hz), Duty (1, TR, uint32, 0-100 %).
struct PWMPersistent { uint32_t PWMFreq = 25000; }; // offset 0
struct PWMVolatile   { uint32_t Duty = 0; };        // offset 0

bool OnPWMFrequencyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);
bool OnPWMDutyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);

const BlockEntry PWM_Entries[] = {
    { MakeFieldKey(0, 0), 0, { (uint16_t)DataType::Uint32, sizeof(uint32_t), ValueTrigger | ValuePersistent } },
    { MakeFieldKey(1, 0), 0, { (uint16_t)DataType::Uint32, sizeof(uint32_t), ValueTrigger } },
};

const BlockTrigger PWM_Triggers[] = {
    { MakeFieldKey(0, 0), OnPWMFrequencyChange },
    { MakeFieldKey(1, 0), OnPWMDutyChange },
};

const BlockSchema PWM_Schema = {
    .Entries = PWM_Entries,
    .EntryCount = sizeof(PWM_Entries) / sizeof(BlockEntry),
    .Triggers = PWM_Triggers,
    .TriggerCount = sizeof(PWM_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::PWM,
};
