#pragma once
#include "Core/Functions/Packet.h"
// Subscriptions service per Docs/Services/Subscriptions.md
// TODO: Active until canceled. Sending device keeps register: Address | TRID | Source Register | Last sent | Len | Trigger type | Trigger info
// Mandatory for sensor nodes, Tamu v2.0A has it per Devices.md but under-specified.
// Stubbed for Tamu v2.0A rebuild - keep space for DAS sensor nodes.
// Current build: silently drop, report via Issues.md.
#if 0
void HandleSubscriptions(const PacketFrame &frame) {
    // Trigger types: Periodic, OnChange, Delta (int vs Number)
}
#endif
