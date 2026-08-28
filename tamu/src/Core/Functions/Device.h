#pragma once

#include "Blocks/DeviceInfo.h"

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

// Global device name string (set per device, e.g. Main.cpp)
extern const char* DeviceName;
extern char DeviceNameBuffer[24];
// Device name persistence (Docs/Services/Device service.md: "Device name is stored in
// standalone file to allow persistence"). Implemented in Core/Services/Device.h.
// Call LoadPersistedDeviceName() at boot once storage is ready so the persisted name
// is active before BLE advertising starts; PersistDeviceName() saves a rename.
void LoadPersistedDeviceName();
bool PersistDeviceName();
// Device software version string (provided per device, e.g. Devices/<device>/Main.h)
extern const char* DeviceVersion;

// Identify (Device service CID 2, Docs/Services/Device service.md): "True = blink red
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
        DeviceIdentifyUntil() = now_ms + 3000; // blink for ~3 s
}
inline bool DeviceIdentifyActive(uint32_t now_ms)
{
    bool &requested = DeviceIdentifyRequested();
    if (requested && (int32_t)(now_ms - DeviceIdentifyUntil()) >= 0)
        requested = false; // window expired
    return requested;
}

