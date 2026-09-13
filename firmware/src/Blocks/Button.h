#pragma once

// Edge detection modes for button blocks (Docs/Modules and blocks/Buttons & LEDS.md):
// the edge counter increments once per detected transition and wraps on uint8 overrun.
enum ButtonEdgeMode : uint8_t {
    EdgeNone   = 0,
    EdgeRising = 1,  // free -> pressed
    EdgeFalling = 2, // pressed -> free
    EdgeBoth   = 3,  // any transition
};

// ===== Button =====
// Docs/Modules and blocks/Buttons & LEDS.md:
//   Button raw state (0, RO, bool), Edge detection (1, P, enum), Edge counter (2, RO, uint8).
struct ButtonStruct {
    bool ButtonState = false;
    uint8_t EdgeDetection = EdgeNone;
    uint8_t EdgeCounter = 0;
};

const BlockMeta Button_Map[] = {
    {DataType::Bool | FieldFlags::ReadOnly, 0x00, sizeof(bool)},
    {DataType::Enum | FieldFlags::Persistent, 0x00, sizeof(uint8_t)},
    {DataType::Index | FieldFlags::ReadOnly, 0x00, sizeof(uint8_t)},
};

const FieldTrigger Button_Triggers[] = {
    nullptr,
    nullptr,
    nullptr,
};

const uint16_t Button_Offsets[] = {0, 1, 2};

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
//   Button raw state (0, RO, bool), Edge detection (1, P, enum), Edge counter
//   (2, RO, uint8), LEDState (3, TR, bool).
struct LEDButtonStruct {
    bool ButtonState = false; // offset 0
    uint8_t EdgeDetection = EdgeNone; // offset 1
    uint8_t EdgeCounter = 0;  // offset 2
    bool LEDState = false;    // offset 3
};

const BlockMeta LEDButton_Map[] = {
    {DataType::Bool | FieldFlags::ReadOnly, 0x00, sizeof(bool)},
    {DataType::Enum | FieldFlags::Persistent, 0x00, sizeof(uint8_t)},
    {DataType::Index | FieldFlags::ReadOnly, 0x00, sizeof(uint8_t)},
    {DataType::Bool | FieldFlags::Trigger, 0x00, sizeof(bool)},
};

bool OnLEDStateChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len);

const FieldTrigger LEDButton_Triggers[] = {
    nullptr,
    nullptr,
    nullptr,
    OnLEDStateChange,
};

const uint16_t LEDButton_Offsets[] = {0, 1, 2, 3};

const BlockSchema LEDButton_Schema = {
    .Map = LEDButton_Map,
    .Triggers = LEDButton_Triggers,
    .Offsets = LEDButton_Offsets,
    .Type = BlockType::LEDButton,
    .MapCount = sizeof(LEDButton_Map) / sizeof(BlockMeta),
};