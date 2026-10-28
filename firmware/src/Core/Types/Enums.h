#pragma once

// The type vocabulary shared by the register, packet and block layers. Self-contained: the enums
// and the App Active bit masks below are spelled in <cstdint> types, so this header owns the
// include rather than relying on whichever header happened to come first.
#include <cstdint>

enum class DeviceType : uint16_t {
    Unknown         = 0x00,
    Tamu_v2_0A      = 0x01,
    Valu_v2_0       = 0x02, // CH32V203G8R6 (Docs/Devices.md)
    DualAnalogSensor = 0x03,
};

namespace Capabilities {
    constexpr uint32_t Core           = 1u << 0;
    // 1u << 2 was the CLI capability; the bit stays reserved so no other bit moves.
    constexpr uint32_t DynamicMemory  = 1u << 3;
    constexpr uint32_t Scripts        = 1u << 4;
    // 1u << 5 was the StorageFiles capability; obsolete (every device has storage files).
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

// System block field 8 "App Active" (Docs/Services/System Block and Device Commands.md) is a BIT
// FIELD, not an enum index: one bit per app interface, and several may be set at once (the app on
// USB while a phone is on BLE). It used to be an `enum class AppActive { None, USB, BLE }`, which
// could only ever name ONE active interface - a device with both links up had to pick a winner
// and report something that was not true.
//
// The wire type stays one byte (DataType::Enum, size 1) so a reader written against the old enum
// still decodes it: it sees a small integer, `1` for USB, `2` for BLE and `3` for both. Nothing
// about the field number or its width changed; only what the byte means. The app labels the bits
// (app/lib/core/app_active.dart), so that numbering lives in one place on each side.
namespace AppActiveBits {
constexpr uint8_t USB  = 1u << 0; // the app is attached over USB
constexpr uint8_t BLE  = 1u << 1; // a phone is connected over BLE
// Reserve: the WiFi app link (the UDP transport in Docs/Services/App Interface.md) is not
// implemented, so bit 2 is never set today. It is named here so the app can label the bit the
// moment the firmware starts reporting it.
constexpr uint8_t WiFi = 1u << 2;

// The field's byte for the two links that exist: set at most these two bits, never WiFi.
constexpr uint8_t Of(bool Usb, bool Ble) { return (uint8_t)((Usb ? USB : 0u) | (Ble ? BLE : 0u)); }
} // namespace AppActiveBits

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
    Text           = 0x0A,  // variable-size character array
    Filename       = 0x0B,
    Enum           = 0x0C,
    Uint32         = 0x0E,
    DevType        = 0x0F,
    BlockInfo      = 0x10,  // 32-bit register pointer (type|inst|field|key)
    Char           = 0x11,  // one character, an element of a string
    Name           = 0x12,  // 16-character reusable type (any 16-char name)
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
    LED            = 0x0A,
    BleSettings    = 0x0B, // BLE link settings + state (Blocks/BleSettings.h)
    WifiSettings   = 0x0C  // WiFi link settings + state (Blocks/WifiSettings.h; compile-in option)
    // The banked Dynamic (0x3F0-0x3F3) and Scripts (0x3F4-0x3F7) ranges are not enum members:
    // they are addressed by BlockTypeRange (Core/Services/RegisterDefs.h).
};

