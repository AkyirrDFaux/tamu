#pragma once

#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h" // MakeId + the DeviceStatus extern

// Payload for address assignment exchange (Discover Response)
struct AssignPayload
{
    SerialNumber sn;
    uint16_t new_addr;
} __attribute__((packed));

// Formats a serial number as a null-terminated hex string (needs 2*sizeof(bytes)+1 chars).
inline void SerialNumberToString(const SerialNumber &sn, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < sizeof(sn.bytes) * 2 + 1)
        return;
    for (size_t i = 0; i < sizeof(sn.bytes); i++)
    {
        buffer[i * 2] = "0123456789ABCDEF"[sn.bytes[i] >> 4];
        buffer[i * 2 + 1] = "0123456789ABCDEF"[sn.bytes[i] & 0x0F];
    }
    buffer[sizeof(sn.bytes) * 2] = '\0'; // Null terminate
}

// Device identity is mandatory for every device build. Each device must provide the
// device-type and capability constants and the serial-number getter; omitting any of them
// fails the build (undefined reference).
// Define them in the device's Main.h, e.g.:
//   extern const DeviceType kDeviceType = DeviceType::Tamu_v2_0A;
//   extern const uint32_t kCapabilities = 0;
//   const SerialNumber& GetSerialNumber() { ... }
extern const DeviceType kDeviceType;
extern const uint32_t kCapabilities;
const SerialNumber &GetSerialNumber();

// Set when Core-discover detects another core with the SAME net-ID on the bus. The core
// stays reachable via the app link (so the net can be changed) but blinks its error LED.
inline bool &CoreCollisionFlag()
{
    static bool collision = false;
    return collision;
}

// Core-discover net-collision check (Docs "Core functions"): a reply from a *different* core
// whose net-ID equals ours means the net is claimed twice. `ours` is DeviceStatus.NetId, the
// peer's net is decoded from its address (MakeId). A match latches CoreCollisionFlag, aborting
// normal boot; the caller ignores our own echoed discover reply.
inline void NoteCoreNet(uint8_t peer_net, uint8_t ours)
{
    if (peer_net != 0 && peer_net == ours)
        CoreCollisionFlag() = true;
}

// True when this device is acting as the core of its net. A core's full address is
// MakeId(NetId, 1): its device field is 1 (SNDB reserves short ID 1, allocation starts at
// 2) and its net is a valid, non-broadcast net. A node's device field is >= 2, so it is
// never mistaken for a core.
inline bool DeviceIsCore()
{
    uint8_t net = (uint8_t)((DeviceStatus.ShortAddress >> 10) & 0x3F);
    return (DeviceStatus.ShortAddress & 0x3FF) == 1 && net != 0 && net != 0x3F;
}

// Identify (Device service CID 2, Docs/Services/System Block and Device Commands.md): "True = blink red
// led fast, False = leave led alone". The flag is set by the Device service handler and
// expires after a short window; each device's main loop blinks its red/notification LED
// while DeviceIdentifyActive() is true. Function-local statics in inline functions are
// guaranteed to be a single shared instance across all translation units.
inline bool &DeviceIdentifyRequested()
{
    static bool requested = false;
    return requested;
}
inline uint32_t &DeviceIdentifyUntil()
{
    static uint32_t until = 0;
    return until;
}
inline void DeviceIdentifyStart(bool on, uint32_t now_ms)
{
    DeviceIdentifyRequested() = on;
    if (on)
        DeviceIdentifyUntil() = now_ms + 10000; // blink for ~10 s (docs)
}
inline bool DeviceIdentifyActive(uint32_t now_ms)
{
    bool &requested = DeviceIdentifyRequested();
    if (requested && (int32_t)(now_ms - DeviceIdentifyUntil()) >= 0)
        requested = false; // window expired
    return requested;
}

