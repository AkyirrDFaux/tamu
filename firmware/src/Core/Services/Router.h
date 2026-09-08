#pragma once
#include "Core/Functions/Packet.h"
// Router service per Docs/Services/Router.md
// TODO: Stores ID table per RSBus port in RAM, appends/updates on direction, unknown/broadcast -> all dirs
// Omitted for single-bus devices, not to be implemented yet (no multi-bus hardware)
// Stubbed - keep space for future multi-bus device.
#if 0
void HandleRouter(const PacketFrame &frame) {
}
#endif
