#pragma once

#include <cstring>
#include <cstdint>
#include "Core/Types/Enums.h"
#include "Core/Types/Number.h"

// Device identity and status. These are owned by the Device service (see
// Docs/Services/Device service.md) and are no longer System Memory blocks; they are reported
// through the service functions (Device type, Serial number, Capability, Uptime, Loop Time).

struct SerialNumber{
    uint8_t bytes[14];
    // Equality: compares the ID bytes.
    bool operator==(const SerialNumber& other) const {
        return memcmp(this->bytes, other.bytes, sizeof(bytes)) == 0;
    }

    // Inequality Operator: Often good practice to include if defining ==
    bool operator!=(const SerialNumber& other) const {
        return !(*this == other);
    }
} __attribute__((packed));

// Device identity is mandatory per build: the device-type/capability constants and the
// GetSerialNumber() getter are declared in Core/Functions/Device.h and defined per device
// (see Docs/Services/Device service.md).

struct DeviceStatusStruct
{
    uint32_t UptimeMs;
    Number AvgLoopTimeMs;
    Number MaxLoopTimeMs;
    uint16_t ShortAddress;
};
