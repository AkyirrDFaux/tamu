#pragma once

enum AccGyrError : uint16_t {
    ErrNone            = 0x0000,
    ErrBusGeneric      = 0x0001, // Failed to transmit/receive
    ErrDeviceNotFound  = 0x0002, // ACK failure (pullups/power)
    ErrInitFailed      = 0x0003, // Reset/Config sequence failed
    ErrTimeout         = 0x0004  // Transaction took too long
};

// Acc&Gyr (Docs/Modules and blocks/Measurement.md): accelerometer + gyroscope combo.
// The ODR / full-scale fields are enums storing the INDEX into the device's supported
// option tables (below); the write triggers reconfigure the sensor and snap the stored
// value to the index actually applied.
//
// | Name                | F.K:SP | Flags | Size    | Note      |
// |---------------------|--------|-------|---------|-----------|
// | Sampling Rate       | 0      | TR,P  | Enum    | Hz        |
// | Range Acceleration  | 1      | TR,P  | Enum    | m/s^2     |
// | Range Angular       | 2      | TR,P  | Enum    | rad/s     |
// | Acceleration Filter | 3      | P     | Number  | EMA (0-1) |
// | Angular Filter      | 4      | P     | Number  | EMA (0-1) |
// | Acceleration        | 5      | RO    | Vector3 | m/s^2     |
// | Angular Velocity    | 6      | RO    | Vector3 | rad/s     |

// Supported output data rates (Hz) - LSM6DS3 CTRL1_XL/CTRL2_G ODR codes 0b0001..0b1000.
enum AccGyrOdr : uint8_t {
    Odr12_5 = 0, Odr26, Odr52, Odr104, Odr208, Odr416, Odr833, Odr1660,
};

// Accel full-scale options (g) mapped to the LSM6DS3TR CTRL1_XL FS_XL codes
// {00, 10, 11, 01} (datasheet Table 51).
enum AccGyrRangeAcc : uint8_t {
    Acc2g = 0, Acc4g, Acc8g, Acc16g,
};

// Gyro full-scale options (deg/s) mapped to the LSM6DS3TR CTRL2_G FS_G codes plus the
// FS_125 bit (datasheet Table 54): {00+FS_125, 00, 01, 10, 11}. No +-4000 dps range
// exists on this part.
enum AccGyrRangeAng : uint8_t {
    Ang125 = 0, Ang250, Ang500, Ang1000, Ang2000,
};

struct AccGyrStruct {
    uint8_t SamplingRate = Odr104; // offset 0
    uint8_t RangeAcc = Acc16g;     // offset 1 (boot default matches the factory config)
    uint8_t RangeAng = Ang2000;    // offset 2
    Number AccFilter = N(1);       // offset 4, EMA coefficient 0-1 (1 = no filtering)
    Number AngFilter = N(1);       // offset 8
    Vector<3> Acceleration;        // offset 12
    Vector<3> AngularVelocity;     // offset 24
};

// Lock the layout: the schema offsets must match the natural C struct alignment.
static_assert(offsetof(AccGyrStruct, SamplingRate) == 0, "AccGyr layout");
static_assert(offsetof(AccGyrStruct, AccFilter) == 4, "AccGyr layout");
static_assert(offsetof(AccGyrStruct, Acceleration) == 12, "AccGyr layout");
static_assert(offsetof(AccGyrStruct, AngularVelocity) == 24, "AccGyr layout");

const BlockMeta AccGyr_Map[] = {
    {DataType::Enum | FieldFlags::Trigger | FieldFlags::Persistent, 0x00, sizeof(uint8_t)},
    {DataType::Enum | FieldFlags::Trigger | FieldFlags::Persistent, 0x00, sizeof(uint8_t)},
    {DataType::Enum | FieldFlags::Trigger | FieldFlags::Persistent, 0x00, sizeof(uint8_t)},
    {DataType::Number | FieldFlags::Persistent, 0x00, sizeof(Number)},
    {DataType::Number | FieldFlags::Persistent, 0x00, sizeof(Number)},
    {DataType::Vector | FieldFlags::ReadOnly, 0x00, sizeof(Vector<3>)},
    {DataType::Vector | FieldFlags::ReadOnly, 0x00, sizeof(Vector<3>)},
};

bool OnAccGyrFrequencyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);
bool OnAccGyrAccRangeChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);
bool OnAccGyrAngRangeChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);

const FieldTrigger AccGyr_Triggers[] = {
    OnAccGyrFrequencyChange,
    OnAccGyrAccRangeChange,
    OnAccGyrAngRangeChange,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

const uint16_t AccGyr_Offsets[] = {0, 1, 2, 4, 8, 12, 24};

const BlockSchema AccGyr_Schema = {
    .Map = AccGyr_Map,
    .Triggers = AccGyr_Triggers,
    .Offsets = AccGyr_Offsets,
    .Type = BlockType::AccGyr,
    .MapCount = sizeof(AccGyr_Map) / sizeof(BlockMeta),
};
