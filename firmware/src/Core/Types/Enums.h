#pragma once

enum class DeviceType : uint16_t {
    Unknown         = 0x00,
    Tamu_v2_0A      = 0x01,
    DualAnalogSensor = 0x03,
};

namespace Capabilities {
    constexpr uint32_t Core           = 1u << 0;
    // 1u << 2 was the CLI capability; the bit stays reserved so no other bit moves.
    constexpr uint32_t DynamicMemory  = 1u << 3;
    constexpr uint32_t Scripts        = 1u << 4;
    constexpr uint32_t StorageFiles   = 1u << 5; // full file create/delete/rename/resize
    constexpr uint32_t AppInterface   = 1u << 6;
    constexpr uint32_t SubscriptionRequest = 1u << 7;
    constexpr uint32_t Node           = 1u << 8;
    constexpr uint32_t SubscriptionProvide = 1u << 9;
}

enum class TriggerType : uint8_t {
    None               = 0, // canceled/to be deleted
    Periodic           = 1,
    OnChangePeriodic   = 2,
    OnChangeConfirm    = 3,
    EdgeRising         = 4,
    EdgeFalling        = 5,
    EdgeAny            = 6,
    DeltaPeriodic      = 7
};

// System block field 8 "App Active" (Docs/Services/System Block and Device Commands.md).
enum class AppActive : uint8_t {
    None = 0, // no app attached
    USB  = 1,
    BLE  = 2,
};

enum class DataType : uint16_t {
    None           = 0x00,
    Undefined      = 0x01,
    SN             = 0x02,
    Id             = 0x03,
    Bool           = 0x04,
    Index          = 0x05,
    Number         = 0x06,
    Vector         = 0x07,
    Matrix         = 0x08,
    Colour         = 0x09,
    String         = 0x0A,
    Filename       = 0x0B,
    Enum           = 0x0C,
    Deleted        = 0x0D,
    Uint32         = 0x0E,
    DevType        = 0x0F,
    BlockInfo      = 0x10,  // 32-bit register pointer (type|inst|field|key)
    // Dictionary/texture extensions (not used by DAS)
    UnknownKeyed   = 0x100,  // generic dictionary marker (Docs/Data Formats.md 0x0100)
    Geometry       = 0x101,
    Texture        = 0x102
};

enum class BlockType : uint16_t {
    None           = 0x00,  // tombstone: no block here; stable until save compacts
    Undefined      = 0x01,  // valid block, type not yet specified
    System         = 0x00,  // System block (type 0, inst 0 in Register service)
    LEDButton      = 0x03,
    PWM            = 0x04,
    AccGyr         = 0x05,
    Vysi1Display   = 0x06,
    Deleted        = 0x07,
    ResistiveMeasure = 0x08,
    Button         = 0x09,
    LED            = 0x0A
    // The banked Dynamic (0x3F0-0x3F3) and Scripts (0x3F4-0x3F7) ranges are not enum members:
    // they are addressed by BlockTypeRange (Core/Services/RegisterDefs.h).
};

