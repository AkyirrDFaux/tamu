#pragma once

// ===== LED =====
// Docs/Modules and blocks/Buttons & LEDS.md:
//   LEDState (0, TR, bool) - true = on, false = off.
struct LEDVolatile {
    bool LEDState = false;
};

// OnLEDStateChange is declared once in Blocks/Button.h, which must be included before this
// header (see Devices/DAS_v0.1/Main.h).

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
