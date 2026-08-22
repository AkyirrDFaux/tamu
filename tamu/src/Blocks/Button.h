struct LEDButtonStruct
{
    bool LEDState = false;
    uint8_t Pad[3] = {0, 0, 0}; // Static block fields are 4-byte aligned: keep
    bool ButtonState = false;   // ButtonState at offset 4 (matching Get()).
};

const BlockMeta LEDButton_Map[] = {
    {DataType::Bool | FieldFlags::None,     0x00, sizeof(bool)},
    {DataType::Bool | FieldFlags::ReadOnly, 0x00, sizeof(bool)},
};

// Callback invoked when the LED state field changes.
bool OnLEDStateChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len);

const TriggerEntry led_callbacks[] = {
    {OnLEDStateChange, 0}};

const BlockSchema LEDButton_Schema = {
    .Map = LEDButton_Map,
    .Triggers = led_callbacks,
    .Type = BlockType::LEDButton,
    .MapCount = sizeof(LEDButton_Map) / sizeof(BlockMeta),
    .TriggerCount = 1};