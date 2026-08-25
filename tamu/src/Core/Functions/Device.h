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

