#pragma once

// ===== Button =====
// Docs/Modules and blocks/Buttons & LEDS.md:
//   Button raw state (0, RO, bool) - true = pressed, false = free.
struct ButtonStruct {
    bool ButtonState = false;
};

const BlockMeta Button_Map[] = {
    {DataType::Bool | FieldFlags::ReadOnly, 0x00, sizeof(bool)},
};

const FieldTrigger Button_Triggers[] = {
    nullptr,
};

const uint16_t Button_Offsets[] = {0};

const BlockSchema Button_Schema = {
    .Map = Button_Map,
    .Triggers = Button_Triggers,
    .Offsets = Button_Offsets,
    .Type = BlockType::Button,
    .MapCount = sizeof(Button_Map) / sizeof(BlockMeta),
};

// ===== LED-Button =====
// Docs/Modules and blocks/Buttons & LEDS.md: button + LED on one pin. The LED is
// controlled by the LEDState field; the button is reported through the Button field
// (if the LED is on, button reading is disabled).
//   Button raw state (0, RO, bool), LEDState (3, TR, bool).
// Fields 1-2 are reserved (None) so LEDState keeps its documented field index 3.
struct LEDButtonStruct {
    bool ButtonState = false; // field 0
    bool LEDState = false;    // field 3
};

const BlockMeta LEDButton_Map[] = {
    {DataType::Bool | FieldFlags::ReadOnly, 0x00, sizeof(bool)}, // 0 Button raw state
    {(uint16_t)DataType::None, 0x00, 0},                          // 1 reserved
    {(uint16_t)DataType::None, 0x00, 0},                          // 2 reserved
    {DataType::Bool | FieldFlags::Trigger, 0x00, sizeof(bool)},  // 3 LEDState
};

bool OnLEDStateChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len);

const FieldTrigger LEDButton_Triggers[] = {
    nullptr,
    nullptr,
    nullptr,
    OnLEDStateChange,
};

// Reserved fields point at the struct start; their Size is 0 so nothing is read/written.
const uint16_t LEDButton_Offsets[] = {0, 0, 0, 1};

const BlockSchema LEDButton_Schema = {
    .Map = LEDButton_Map,
    .Triggers = LEDButton_Triggers,
    .Offsets = LEDButton_Offsets,
    .Type = BlockType::LEDButton,
    .MapCount = sizeof(LEDButton_Map) / sizeof(BlockMeta),
};
