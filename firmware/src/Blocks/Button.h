#pragma once

// ===== Button =====
// Docs/Modules and blocks/Buttons & LEDS.md:
//   Button raw state (0, RO, bool) - true = pressed, false = free.
struct ButtonVolatile {
    bool ButtonState = false;
};

const BlockEntry Button_Entries[] = {
    { MakeFieldKey(0, 0), 0, {(uint16_t)DataType::Bool, sizeof(bool), ValueReadOnly} },
};

const BlockSchema Button_Schema = {
    .Entries = Button_Entries,
    .EntryCount = sizeof(Button_Entries) / sizeof(BlockEntry),
    .Triggers = nullptr,
    .TriggerCount = 0,
    .Type = BlockType::Button,
};

// ===== LED-Button =====
// Docs/Modules and blocks/Buttons & LEDS.md: button + LED on one pin. The LED is
// controlled by the LEDState field; the button is reported through the Button field
// (if the LED is on, button reading is disabled).
//   Button raw state (0, RO, bool), LEDState (3, TR, bool).
// Fields 1-2 are reserved and omitted from the literal table, so LEDState keeps its
// documented field index 3.
struct LEDButtonVolatile {
    bool ButtonState = false; // field 0
    bool LEDState = false;    // field 3
};

bool OnLEDStateChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len);

const BlockEntry LEDButton_Entries[] = {
    { MakeFieldKey(0, 0), 0, {(uint16_t)DataType::Bool, sizeof(bool), ValueReadOnly} }, // Button raw state
    { MakeFieldKey(3, 0), 1, {(uint16_t)DataType::Bool, sizeof(bool), ValueTrigger} },   // LEDState
};

const BlockTrigger LEDButton_Triggers[] = {
    { MakeFieldKey(3, 0), OnLEDStateChange },
};

const BlockSchema LEDButton_Schema = {
    .Entries = LEDButton_Entries,
    .EntryCount = sizeof(LEDButton_Entries) / sizeof(BlockEntry),
    .Triggers = LEDButton_Triggers,
    .TriggerCount = sizeof(LEDButton_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::LEDButton,
};
