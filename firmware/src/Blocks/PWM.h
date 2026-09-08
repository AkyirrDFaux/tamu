#pragma once

struct PWMStruct {
    uint32_t PWMFreq = 25000;
    Number Duty = 0;
};

const BlockMeta PWM_Map[] = {
    { DataType::Uint32 | FieldFlags::Trigger | FieldFlags::Persistent, 0x00, sizeof(uint32_t) },
    { DataType::Number | FieldFlags::Trigger, 0x00, sizeof(Number)},
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
