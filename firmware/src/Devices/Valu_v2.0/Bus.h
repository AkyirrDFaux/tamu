#pragma once

// Bus stubs for the Valu v2.0. Docs/Devices.md lists no RSBus/Router service for this board -
// it is a standalone node whose only interface is the USB CDC App Interface. The core's
// dispatcher and Device service still reference these two functions (the bus is a shared
// contract across every build), so they are provided here as no-ops: the Valu originates
// nothing onto a bus and receives nothing from one.

#include "Core/Functions/Packet.h"

// No bus to transmit on: a targeted-foreign frame cannot be forwarded (always fails).
bool SendAndVerifyPacket(const PacketFrame &Data)
{
    (void)Data;
    return false;
}

// No bus to read: there are never any received frames.
int ReceivePacket(PacketFrame *Data)
{
    (void)Data;
    return 0;
}
