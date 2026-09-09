#pragma once

enum class DeviceType : uint16_t {
    Unknown         = 0x00,
    Tamu_v2_0A      = 0x01,
    DualAnalogSensor = 0x03,
};

namespace Capabilities {
    constexpr uint32_t None           = 0x00000000;
    constexpr uint32_t Core           = 1u << 0;
    constexpr uint32_t Router         = 1u << 1;
    constexpr uint32_t Cli            = 1u << 2;
    constexpr uint32_t Scripts        = 1u << 5;
    constexpr uint32_t AppInterface   = 1u << 6;
    constexpr uint32_t Subscriptions  = 1u << 7;
    constexpr uint32_t Node           = 1u << 8;
}

enum FieldFlags : uint16_t {
    None          = 0x0000,
    ReadOnly      = 0x0400,
    Persistent    = 0x0800,
    Trigger       = 0x1000,
    NotSaved      = 0x2000,
    ScriptUpdated = 0x4000,
    External      = 0x8000
};
constexpr uint16_t FlashValid = 0x0400;

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
    NetAddr        = 0x10,
    Unknown        = 0x00,  // Alias for None
    // Dynamic/Keyed extensions (not used by DAS)
    UnknownKeyed   = 0x100,
    GenericDict    = 0x100,
    Geometry       = 0x101,
    Texture        = 0x102,
    Effect         = 0x103
};

#define BLOCK_META_FLAGS_MASK 0xFC00
#define BLOCK_META_TYPE_MASK  0x03FF

inline bool IsKeyedType(DataType Type){
    return ((uint16_t)Type & BLOCK_META_TYPE_MASK) >= (uint16_t)DataType::UnknownKeyed;
}

enum class BlockType : uint16_t {
    None           = 0x00,
    Unknown        = 0x00,
    Undefined      = 0x01,
    System         = 0x00,
    LEDButton      = 0x03,
    PWM            = 0x04,
    AccGyr         = 0x05,
    Vysi1Display   = 0x06,
    Deleted        = 0x07,
    ResistiveMeasure = 0x08,
    Render         = 0x100,
    Scripts        = 0x2FF,
    Dynamic        = 0x3FF
};

constexpr uint16_t operator|(DataType type, FieldFlags flag) {
    return static_cast<uint16_t>(type) | static_cast<uint16_t>(flag);
}
