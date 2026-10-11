#pragma once

#include <cstdint>

// Log/error report sent to the LogHandler service (Docs/Services/Log Handler.md).
// Packed 96-bit layout (the doc's Log Struct):
//   source:    the whole block index - BlockInfo type (10 bits) | instance (6 bits) |
//              field (8 bits) | key (8 bits)  (little-endian u32)
//   code:      source-specific 32-bit code
//   timestamp: synced ms (u32)
//
// A block log fills the source from its block index. A service-originated log has no block
// index: its source's type field is the reserved LOG_SOURCE_SERVICE marker and the instance
// field carries the ServiceType (so the block/service distinction costs no extra bit).
typedef struct __attribute__((packed))
{
    uint32_t Source;
    uint32_t Code;
    uint32_t Timestamp;
} LogMessage;

// Reserved source type marking a service-originated log (the instance field then holds the
// ServiceType). 0x3FF is outside every real BlockType.
constexpr uint16_t LOG_SOURCE_SERVICE = 0x3FF;

// Packs the whole block index from a block type (10 bits), an instance (6 bits) and the
// 16-bit field/key specifier (field 8 bits | key 8 bits), matching the BlockInfo layout
// (Core/Services/RegisterDefs.h).
inline uint32_t MakeLogSource(uint16_t Type, uint8_t Instance, uint8_t Field = 0, uint8_t Key = 0)
{
    return ((uint32_t)(Type & 0x3FF) << 22) | ((uint32_t)(Instance & 0x3F) << 16) |
           ((uint32_t)Field << 8) | (uint32_t)Key;
}

// Builds a LogMessage. `is_block` selects a block log (`type_or_service` is the BlockType and
// `instance` the block instance, completing the block index) or a service log
// (`type_or_service` is the ServiceType, placed in the instance field under
// LOG_SOURCE_SERVICE). `code` is the source-specific 32-bit code.
inline LogMessage MakeLog(bool IsBlock, uint16_t TypeOrService, uint32_t Code,
                          uint32_t Timestamp, uint8_t Instance = 0)
{
    LogMessage Message;
    Message.Source = IsBlock ? MakeLogSource(TypeOrService, Instance)
                        : MakeLogSource(LOG_SOURCE_SERVICE, (uint8_t)TypeOrService);
    Message.Code = Code;
    Message.Timestamp = Timestamp;
    return Message;
}

// The source's block type (or LOG_SOURCE_SERVICE).
inline uint16_t LogSourceType(const LogMessage &Message)
{
    return (uint16_t)((Message.Source >> 22) & 0x3FF);
}

// The source's instance (a block instance, or the ServiceType for a service log).
inline uint8_t LogSourceInstance(const LogMessage &Message)
{
    return (uint8_t)((Message.Source >> 16) & 0x3F);
}

// The source's field (bits 8-15 of the block index).
inline uint8_t LogSourceField(const LogMessage &Message)
{
    return (uint8_t)((Message.Source >> 8) & 0xFF);
}

// The source's key (bits 0-7 of the block index).
inline uint8_t LogSourceKey(const LogMessage &Message)
{
    return (uint8_t)(Message.Source & 0xFF);
}

// True if the source is a block (rather than a service).
inline bool LogIsBlock(const LogMessage &Message)
{
    return LogSourceType(Message) != LOG_SOURCE_SERVICE;
}

// The block type or service type the log came from.
inline uint16_t LogSourceId(const LogMessage &Message)
{
    return LogIsBlock(Message) ? LogSourceType(Message) : LogSourceInstance(Message);
}

// The source-specific 32-bit log code.
inline uint32_t LogCode(const LogMessage &Message)
{
    return Message.Code;
}
