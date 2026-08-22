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
    // Default constructor leaves the 14 bytes uninitialised.
    SerialNumber() = default;
    // Equality: compares the 14 ID bytes.
    bool operator==(const SerialNumber& other) const {
        return memcmp(this->bytes, other.bytes, 14) == 0;
    }

    // Inequality Operator: Often good practice to include if defining ==
    bool operator!=(const SerialNumber& other) const {
        return !(*this == other);
    }

    // Assignment Operator: Copies the 14 bytes from other to this
    SerialNumber& operator=(const SerialNumber& other) {
        if (this != &other) { // Protect against self-assignment
            memcpy(this->bytes, other.bytes, 14);
        }
        return *this;
    }

    // Copy constructor: copies the 14 bytes from `other`.
    SerialNumber(const SerialNumber& other) {
        memcpy(this->bytes, other.bytes, 14);
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
