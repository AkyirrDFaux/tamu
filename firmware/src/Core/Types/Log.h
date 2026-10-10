#pragma once

#include <cstdint>

// Log/error report sent to the LogHandler service (Docs/Services/Log Handler.md).
// Packed 64-bit layout (the doc's Log Struct):
//   source:    BlockInfo type (10 bits) | instance (6 bits)  (little-endian u16)
//   category:  log category (8 bits)
//   specifics: log specifics (8 bits)
//   timestamp: synced ms (u32)
//
// A block log fills the source from its BlockType + instance. A service-originated log has no
// block type/instance: its source's type field is the reserved LOG_SOURCE_SERVICE marker and
// the instance field carries the ServiceType (so the block/service distinction costs no extra
// bit - the full 16-bit source is available for BlockType+Instance).
typedef struct __attribute__((packed))
{
    uint16_t Source;
    uint8_t Category;
    uint8_t Specifics;
    uint32_t Timestamp;
} LogMessage;

// Reserved source type marking a service-originated log (the instance field then holds the
// ServiceType). 0x3FF is outside every real BlockType.
constexpr uint16_t LOG_SOURCE_SERVICE = 0x3FF;

// Packs a source from a block type (10 bits) and an instance (6 bits).
inline uint16_t MakeLogSource(uint16_t Type, uint8_t Instance)
{
    return (uint16_t)((Type & 0x3FF) | ((uint16_t)(Instance & 0x3F) << 10));
}

// Builds a LogMessage. `is_block` selects a block log (`type_or_service` is the BlockType and
// `instance` the block instance) or a service log (`type_or_service` is the ServiceType, placed
// in the instance field under LOG_SOURCE_SERVICE). `code` splits into category (high byte) and
// specifics (low byte), matching the doc's two 8-bit fields.
inline LogMessage MakeLog(bool IsBlock, uint16_t TypeOrService, uint16_t Code,
                          uint32_t Timestamp, uint8_t Instance = 0)
{
    LogMessage m;
    m.Source = IsBlock ? MakeLogSource(TypeOrService, Instance)
                        : MakeLogSource(LOG_SOURCE_SERVICE, (uint8_t)TypeOrService);
    m.Category = (uint8_t)(Code >> 8);
    m.Specifics = (uint8_t)(Code & 0xFF);
    m.Timestamp = Timestamp;
    return m;
}

// True if the source is a block (rather than a service).
inline bool LogIsBlock(const LogMessage &m)
{
    return (m.Source & 0x3FF) != LOG_SOURCE_SERVICE;
}

// The source's block type (or LOG_SOURCE_SERVICE).
inline uint16_t LogSourceType(const LogMessage &m)
{
    return (uint16_t)(m.Source & 0x3FF);
}

// The source's instance (a block instance, or the ServiceType for a service log).
inline uint8_t LogSourceInstance(const LogMessage &m)
{
    return (uint8_t)(m.Source >> 10);
}

// The block type or service type the log came from.
inline uint16_t LogSourceId(const LogMessage &m)
{
    return LogIsBlock(m) ? LogSourceType(m) : LogSourceInstance(m);
}

// The 16-bit code (category << 8 | specifics).
inline uint16_t LogCode(const LogMessage &m)
{
    return (uint16_t)(((uint16_t)m.Category << 8) | m.Specifics);
}
