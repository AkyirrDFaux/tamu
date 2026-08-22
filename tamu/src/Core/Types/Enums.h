#pragma once

enum class DeviceType : uint16_t {
    Unknown    = 0x00,
    Tamu_v2_0A = 0x01,
    Valu_v2_0   = 0x02,
    DualAnalogSensor = 0x03,
};

enum FieldFlags : uint16_t {
    None          = 0x0000,
    Valid         = 0x0400, // Flash only: if 0, a newer version exists, ignore this entry
    ReadOnly      = 0x1000,
    NotSaved      = 0x2000,
    ScriptUpdated = 0x4000,
    RemoteOrigin  = 0x8000
};

enum class DataType : uint16_t {
    Unknown = 0x00,
    SN      = 0x001,
    Uint32  = 0x002,
    Number  = 0x003,
    DevType = 0x004,
    NetAddr = 0x005,
    Bool    = 0x006,
    Vector = 0x007,
    Matrix = 0x008,
    Enum = 0x009,
    Colour = 0x00A,
    Index  = 0x00B,   // 32bit signed integer
    String = 0x00C,   // Text (8bit per character)
    Deleted = 0x00D,  // Pending change, deallocated only after save
    UnknownKeyed = 0x100,
    Geometry = 0x101,
    Texture = 0x102
};

// BlockMeta: Flags in bits 10-15, Type in bits 0-9 (Data Formats.md)
#define BLOCK_META_FLAGS_MASK 0xFC00
#define BLOCK_META_TYPE_MASK  0x03FF

// Returns true if `Type` is a keyed data type (value >= UnknownKeyed)
inline bool IsKeyedType(DataType Type){
    return ((uint16_t)Type & BLOCK_META_TYPE_MASK) >= (uint16_t)DataType::UnknownKeyed;
}

enum class BlockType : uint16_t {
    Unknown  = 0x00,
    LEDButton = 0x03,
    PWM = 0x04,
    AccGyr = 0x05,
    Vysi1Display = 0x06,
    Deleted  = 0x07,
    ResistiveMeasure = 0x08,
    Render  = 0x100
};

// Helper for bitwise packing
constexpr uint16_t operator|(DataType type, FieldFlags flag) {
    return static_cast<uint16_t>(type) | static_cast<uint16_t>(flag);
}

