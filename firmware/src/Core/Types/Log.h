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
    uint16_t source;
    uint8_t category;
    uint8_t specifics;
    uint32_t timestamp;
} LogMessage;

// Reserved source type marking a service-originated log (the instance field then holds the
// ServiceType). 0x3FF is outside every real BlockType.
constexpr uint16_t LOG_SOURCE_SERVICE = 0x3FF;

// Packs a source from a block type (10 bits) and an instance (6 bits).
inline uint16_t MakeLogSource(uint16_t type, uint8_t instance)
{
    return (uint16_t)((type & 0x3FF) | ((uint16_t)(instance & 0x3F) << 10));
}

// Builds a LogMessage. `is_block` selects a block log (`type_or_service` is the BlockType and
// `instance` the block instance) or a service log (`type_or_service` is the ServiceType, placed
// in the instance field under LOG_SOURCE_SERVICE). `code` splits into category (high byte) and
// specifics (low byte), matching the doc's two 8-bit fields.
inline LogMessage MakeLog(bool is_block, uint16_t type_or_service, uint16_t code,
                          uint32_t timestamp, uint8_t instance = 0)
{
    LogMessage m;
    m.source = is_block ? MakeLogSource(type_or_service, instance)
                        : MakeLogSource(LOG_SOURCE_SERVICE, (uint8_t)type_or_service);
    m.category = (uint8_t)(code >> 8);
    m.specifics = (uint8_t)(code & 0xFF);
    m.timestamp = timestamp;
    return m;
}

// True if the source is a block (rather than a service).
inline bool LogIsBlock(const LogMessage &m)
{
    return (m.source & 0x3FF) != LOG_SOURCE_SERVICE;
}

// The source's block type (or LOG_SOURCE_SERVICE).
inline uint16_t LogSourceType(const LogMessage &m)
{
    return (uint16_t)(m.source & 0x3FF);
}

// The source's instance (a block instance, or the ServiceType for a service log).
inline uint8_t LogSourceInstance(const LogMessage &m)
{
    return (uint8_t)(m.source >> 10);
}

// The block type or service type the log came from.
inline uint16_t LogSourceId(const LogMessage &m)
{
    return LogIsBlock(m) ? LogSourceType(m) : LogSourceInstance(m);
}

// The 16-bit code (category << 8 | specifics).
inline uint16_t LogCode(const LogMessage &m)
{
    return (uint16_t)(((uint16_t)m.category << 8) | m.specifics);
}
