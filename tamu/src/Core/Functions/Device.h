#pragma once

#include "Blocks/DeviceInfo.h"

// Payload for address assignment exchange (Discover Response)
struct AssignPayload
{
    SerialNumber sn;
    uint16_t new_addr;
} __attribute__((packed));

// Formats a 14-byte serial number as a null-terminated hex string.
void SerialNumberToString(const SerialNumber &sn, char *buffer)
{
    for (int i = 0; i < 14; i++)
    {
        buffer[i * 2] = "0123456789ABCDEF"[sn.bytes[i] >> 4];
        buffer[i * 2 + 1] = "0123456789ABCDEF"[sn.bytes[i] & 0x0F];
    }
    buffer[28] = '\0'; // Null terminate
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
// Device software version string (provided per device, e.g. Devices/<device>/Main.h)
extern const char* DeviceVersion;

