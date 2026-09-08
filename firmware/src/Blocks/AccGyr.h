#pragma once

enum AccGyrError : uint16_t {
    ErrNone            = 0x0000,
    ErrBusGeneric      = 0x0001, // Failed to transmit/receive
    ErrDeviceNotFound  = 0x0002, // ACK failure (pullups/power)
    ErrInitFailed      = 0x0003, // Reset/Config sequence failed
    ErrTimeout         = 0x0004  // Transaction took too long
};

struct AccGyrStruct {
    Number SamplingRate;
    Vector<3> Acceleration;
    Vector<3> AngularVelocity;
    Number AccFilter;
    Number AngFilter;
};

const BlockMeta AccGyr_Map[] = {
    { DataType::Number | FieldFlags::Trigger | FieldFlags::Persistent, 0x00, sizeof(Number) },
    { DataType::Vector | FieldFlags::ReadOnly, 0x00, sizeof(Vector<3>)},
    { DataType::Vector | FieldFlags::ReadOnly, 0x00, sizeof(Vector<3>)},
    { DataType::Number | FieldFlags::Persistent, 0x00, sizeof(Number) },
    { DataType::Number | FieldFlags::Persistent, 0x00, sizeof(Number) },
};

bool OnAccGyrFrequencyChange(const StaticBlockDescriptor& block, uint16_t index, const void* data, uint16_t data_len);

const FieldTrigger AccGyr_Triggers[] = {
    OnAccGyrFrequencyChange,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

const uint16_t AccGyr_Offsets[] = {0, 4, 16, 28, 32};

const BlockSchema AccGyr_Schema = {
    .Map = AccGyr_Map,
    .Triggers = AccGyr_Triggers,
    .Offsets = AccGyr_Offsets,
    .Type = BlockType::AccGyr,
    .MapCount = sizeof(AccGyr_Map) / sizeof(BlockMeta),
};
