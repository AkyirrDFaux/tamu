#pragma once

// ===== LED =====
// Docs/Modules and blocks/Buttons & LEDS.md:
//   LEDState (0, TR, bool) - true = on, false = off.
struct LEDStruct {
    bool LEDState = false;
};

const BlockMeta LED_Map[] = {
    {DataType::Bool | FieldFlags::Trigger, 0x00, sizeof(bool)},
};

bool OnLEDStateChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len);

const FieldTrigger LED_Triggers[] = {
    OnLEDStateChange,
};

const uint16_t LED_Offsets[] = {0};

const BlockSchema LED_Schema = {
    .Map = LED_Map,
    .Triggers = LED_Triggers,
    .Offsets = LED_Offsets,
    .Type = BlockType::LED,
    .MapCount = sizeof(LED_Map) / sizeof(BlockMeta),
};