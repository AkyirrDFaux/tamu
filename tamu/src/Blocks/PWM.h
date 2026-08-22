struct PWMStruct {
    uint32_t PWMFreq = 25000;
    Number Duty = 0;
};

const BlockMeta PWM_Map[] = {
    { DataType::Uint32 | FieldFlags::None, 0x00, sizeof(uint32_t) },
    { DataType::Number | FieldFlags::None, 0x00, sizeof(Number)},
};

// Callback invoked when the PWM frequency field changes.
bool OnPWMFrequencyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);
// Callback invoked when the PWM duty cycle field changes.
bool OnPWMDutyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);


const TriggerEntry PWM_callbacks[] = {
    { OnPWMFrequencyChange, 0 },
    { OnPWMDutyChange, 1 }
};

const BlockSchema PWM_Schema = {
    .Map = PWM_Map,
    .Triggers = PWM_callbacks,
    .Type = BlockType::PWM,
    .MapCount = sizeof(PWM_Map) / sizeof(BlockMeta),
    .TriggerCount = 2
};