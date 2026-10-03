#pragma once

// ===== LED =====
// Docs/Modules and blocks/Buttons & LEDS.md:
//   LEDState (0, TR, bool) - true = on, false = off.
struct LEDVolatile {
    bool LEDState = false;
};

bool OnLEDStateChange(const StaticBlockDescriptor &block, uint16_t index, const void *data, uint16_t data_len);

const BlockEntry LED_Entries[] = {
    { MakeFieldKey(0, 0), 0, {(uint16_t)DataType::Bool, sizeof(bool), ValueTrigger} },
};

const BlockTrigger LED_Triggers[] = {
    { MakeFieldKey(0, 0), OnLEDStateChange },
};

const BlockSchema LED_Schema = {
    .Entries = LED_Entries,
    .EntryCount = sizeof(LED_Entries) / sizeof(BlockEntry),
    .Triggers = LED_Triggers,
    .TriggerCount = sizeof(LED_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::LED,
};
