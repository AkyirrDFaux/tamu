#pragma once

#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h" // MakeId + the DeviceStatus extern

// Payload for address assignment exchange (Discover Response)
struct AssignPayload
{
    SerialNumber Sn;
    uint16_t NewAddr;
} __attribute__((packed));

// Formats a serial number as a null-terminated hex string (needs 2*sizeof(bytes)+1 chars).
inline void SerialNumberToString(const SerialNumber &Sn, char *Buffer, size_t BufferSize)
{
    if (!Buffer || BufferSize < sizeof(Sn.Bytes) * 2 + 1)
        return;
    for (size_t i = 0; i < sizeof(Sn.Bytes); i++)
    {
        Buffer[i * 2] = "0123456789ABCDEF"[Sn.Bytes[i] >> 4];
        Buffer[i * 2 + 1] = "0123456789ABCDEF"[Sn.Bytes[i] & 0x0F];
    }
    Buffer[sizeof(Sn.Bytes) * 2] = '\0'; // Null terminate
}

// Device identity is mandatory for every device build. Each device must provide the
// device-type and capability constants and the serial-number getter; omitting any of them
// fails the build (undefined reference).
// Define them in the device's Main.h, e.g.:
//   extern const DeviceType KDeviceType = DeviceType::Tamu_v2_0A;
//   extern const uint32_t KCapabilities = 0;
//   const SerialNumber& GetSerialNumber() { ... }
extern const DeviceType KDeviceType;
extern const uint32_t KCapabilities;
const SerialNumber &GetSerialNumber();

// Set when Core-discover detects another core with the SAME net-ID on the bus. The core
// stays reachable via the app link (so the net can be changed) but blinks its error LED.
inline bool &CoreCollisionFlag()
{
    static bool Collision = false;
    return Collision;
}

// Core-discover net-collision check (Docs "Core functions"): a reply from a *different* core
// whose net-ID equals ours means the net is claimed twice. `ours` is DeviceStatus.NetId, the
// peer's net is decoded from its address (MakeId). A match latches CoreCollisionFlag, aborting
// normal boot; the caller ignores our own echoed discover reply.
inline void NoteCoreNet(uint8_t PeerNet, uint8_t Ours)
{
    if (PeerNet != 0 && PeerNet == Ours)
        CoreCollisionFlag() = true;
}

// True when this device is acting as the core of its net. A core's full address is
// MakeId(NetId, 1): its device field is 1 (SNDB reserves short ID 1, allocation starts at
// 2) and its net is a valid, non-broadcast net. A node's device field is >= 2, so it is
// never mistaken for a core.
inline bool DeviceIsCore()
{
    uint8_t Net = (uint8_t)((DeviceStatus.ShortAddress >> 10) & 0x3F);
    return (DeviceStatus.ShortAddress & 0x3FF) == 1 && Net != 0 && Net != 0x3F;
}

// Identify (Device service CID 2, Docs/Services/System Block and Device Commands.md): "True = blink red
// led fast, False = leave led alone". The flag is set by the Device service handler and
// expires after a short window; each device's main loop blinks its red/notification LED
// while DeviceIdentifyActive() is true. Function-local statics in inline functions are
// guaranteed to be a single shared instance across all translation units.
inline bool &DeviceIdentifyRequested()
{
    static bool Requested = false;
    return Requested;
}
inline uint32_t &DeviceIdentifyUntil()
{
    static uint32_t Until = 0;
    return Until;
}
inline void DeviceIdentifyStart(bool On, uint32_t NowMs)
{
    DeviceIdentifyRequested() = On;
    if (On)
        DeviceIdentifyUntil() = NowMs + 10000; // blink for ~10 s (docs)
}
inline bool DeviceIdentifyActive(uint32_t NowMs)
{
    bool &Requested = DeviceIdentifyRequested();
    if (Requested && (int32_t)(NowMs - DeviceIdentifyUntil()) >= 0)
        Requested = false; // window expired
    return Requested;
}

// Enter-bootloader request (Device service CID 4, "Bootloader": the inverse of the bootloader's
// jump-to-app). A device whose image has a bootloader to return to defines
// DEVICE_HAS_BOOTLOADER_ENTRY (platformio.ini) and provides this hook: the Device service handler
// sends the reply and then calls it, and it hands the node to the bootloader. Without the define
// the handler's case is not compiled at all, so nothing accepts a request it cannot act on.
//
// It MUST NOT RETURN: the bootloader's entry re-initialises its own RAM and serves forever, so an
// implementation that comes back would resume an app whose bus/UART hardware the bootloader has
// already retaken. Declared here (not in the device tree) because the shared handler is what calls
// it; the definition lives in the device's Main.h, next to the address it jumps to.
//
// Enabled on the DAS (2026-11-15): the handler and the jump cost 16 B of .text, paid for by the
// storage default-initialiser diet (Core/Functions/StorageBlockFS.h), and the image ends 8 B
// below .fixed_data. Other targets do not define DEVICE_HAS_BOOTLOADER_ENTRY.
#ifdef DEVICE_HAS_BOOTLOADER_ENTRY
extern "C" __attribute__((noreturn)) void DeviceEnterBootloaderJump(void);
#endif

