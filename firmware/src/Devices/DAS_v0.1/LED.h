#pragma once

// DAS LED block: drives the RED LED (PA1). The white LED (PD0) is the RS485 TX activity
// indicator (see RSBus.h) and is not part of this block. Red has priority overlays
// resolved in the main loop: an active error blinks it, then identify, then the block
// value (Docs/Devices.md, user requirement: "used to indicate errors - that has priority").
#define DAS_LED_PORT GPIOA
#define DAS_LED_PIN  GPIO_Pin_1

// Stores the written LEDState. The pin itself is driven by the main loop, which resolves
// the priority overlays (error > identify > block) so a write can never fight a blink.
bool OnLEDStateChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len)
{
    if (data_len != sizeof(bool)) return false;
    auto *led = static_cast<LEDStruct *>(block.Data);
    led->LEDState = *static_cast<const bool *>(data);
    return true;
}