#pragma once

// Fan output (Docs/Modules and blocks/Generic system blocks.md): simple PWM output on
// the specified pin.
//   Frequency (0, TR, P, uint32, Hz), Duty (1, TR, P, uint32, 0-100 %).
// The duty persists as well: a fan running at a duty has to come back up at it, so the duty
// follows the frequency into the persistent space (.SV) and the block has no volatile half.
struct PWMPersistent { uint32_t PWMFreq = 25000; uint32_t PWMDuty = 0; }; // offsets 0, 4
struct PWMVolatile {};

bool OnPWMFrequencyChange(const StaticBlockDescriptor& Block, uint16_t Index, const void* Data, uint16_t DataLen);
bool OnPWMDutyChange(const StaticBlockDescriptor& Block, uint16_t Index, const void* Data, uint16_t DataLen);

const BlockEntry PWM_Entries[] = {
    { MakeFieldKey(0, 0), 0, { (uint16_t)DataType::Uint32, sizeof(uint32_t), ValueTrigger | ValuePersistent } },
    { MakeFieldKey(1, 0), 4, { (uint16_t)DataType::Uint32, sizeof(uint32_t), ValueTrigger | ValuePersistent } },
};

const BlockTrigger PWM_Triggers[] = {
    { MakeFieldKey(0, 0), OnPWMFrequencyChange },
    { MakeFieldKey(1, 0), OnPWMDutyChange },
};

const BlockSchema PWMSchema = {
    .Entries = PWM_Entries,
    .EntryCount = sizeof(PWM_Entries) / sizeof(BlockEntry),
    .Triggers = PWM_Triggers,
    .TriggerCount = sizeof(PWM_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::PWM,
};
