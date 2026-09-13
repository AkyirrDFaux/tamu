#pragma once

// Fan output (Docs/Modules and blocks/Generic system blocks.md): simple PWM output on
// the specified pin.
//   Frequency (0, TR, P, uint32, Hz), Duty (1, TR, uint32, 0-100 %).
struct PWMStruct {
    uint32_t PWMFreq = 25000;
    uint32_t Duty = 0;
};

const BlockMeta PWM_Map[] = {
    { DataType::Uint32 | FieldFlags::Trigger | FieldFlags::Persistent, 0x00, sizeof(uint32_t) },
    { DataType::Uint32 | FieldFlags::Trigger, 0x00, sizeof(uint32_t) },
};

bool OnPWMFrequencyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);
bool OnPWMDutyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);

const FieldTrigger PWM_Triggers[] = {
    OnPWMFrequencyChange,
    OnPWMDutyChange,
};

const uint16_t PWM_Offsets[] = {0, 4};

const BlockSchema PWM_Schema = {
    .Map = PWM_Map,
    .Triggers = PWM_Triggers,
    .Offsets = PWM_Offsets,
    .Type = BlockType::PWM,
    .MapCount = sizeof(PWM_Map) / sizeof(BlockMeta),
};