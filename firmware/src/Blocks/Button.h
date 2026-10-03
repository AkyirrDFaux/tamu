#pragma once

// ===== Button =====
// Docs/Modules and blocks/Buttons & LEDS.md:
//   Button raw state (0, RO, bool) - true = pressed, false = free.
struct ButtonVolatile {
    bool ButtonState = false;
};

const ValueInfo Button_Map[] = {
    {(uint16_t)DataType::Bool, sizeof(bool), ValueReadOnly},
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
    .MapCount = sizeof(Button_Map) / sizeof(ValueInfo),
};

// ===== LED-Button =====
// Docs/Modules and blocks/Buttons & LEDS.md: button + LED on one pin. The LED is
// controlled by the LEDState field; the button is reported through the Button field
// (if the LED is on, button reading is disabled).
//   Button raw state (0, RO, bool), LEDState (3, TR, bool).
// Fields 1-2 are reserved (None) so LEDState keeps its documented field index 3.
struct LEDButtonVolatile {
    bool ButtonState = false; // field 0
    bool LEDState = false;    // field 3
};

const ValueInfo LEDButton_Map[] = {
    {(uint16_t)DataType::Bool, sizeof(bool), ValueReadOnly}, // 0 Button raw state
    {(uint16_t)DataType::None, 0, 0},                         // 1 reserved
    {(uint16_t)DataType::None, 0, 0},                         // 2 reserved
    {(uint16_t)DataType::Bool, sizeof(bool), ValueTrigger},   // 3 LEDState
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
    .MapCount = sizeof(LEDButton_Map) / sizeof(ValueInfo),
};
