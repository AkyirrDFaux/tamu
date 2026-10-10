#pragma once

// DAS LED block: drives the RED LED (PA1). The white LED (PD0) is the RS485 TX activity
// indicator (see RSBus.h) and is not part of this block. Red has priority overlays
// resolved in the main loop: an active error blinks it, then identify, then the block
// value (Docs/Devices.md, user requirement: "used to indicate errors - that has priority").

// Stores the written LEDState. The pin itself is driven by the main loop, which resolves
// the priority overlays (error > identify > block) so a write can never fight a blink.
bool OnLEDStateChange(const StaticBlockDescriptor& Block, uint16_t Index, const void* Data, uint16_t DataLen)
{
    if (DataLen != sizeof(bool)) return false;
    auto *Led = static_cast<LEDVolatile *>(Block.VolatileData);
    Led->LEDState = *static_cast<const bool *>(Data);
    return true;
}