#pragma once

#include <cstdint>

// Log/error report sent to the LogHandler service (Docs/Services/Log Handler.md).
// Packed 64-bit layout:
//   src_and_code: bit0   = source is a Service (0) or a Block (1)
//                 bits1-15 = source block/service ID
//                 bits16-31 = log code (origin specific, usually 8 bit x2)
//   timestamp:   synced ms
typedef struct __attribute__((packed))
{
    uint32_t src_and_code;
    uint32_t timestamp;
} LogMessage;

// Builds a LogMessage from its fields.
inline LogMessage MakeLog(bool is_block, uint16_t source_id, uint16_t code, uint32_t timestamp)
{
    LogMessage m;
    m.src_and_code = ((uint32_t)(is_block ? 1 : 0)) |
                     ((uint32_t)(source_id & 0x7FFF) << 1) |
                     ((uint32_t)code << 16);
    m.timestamp = timestamp;
    return m;
}

// True if the source is a block (rather than a service).
inline bool LogIsBlock(const LogMessage &m)
{
    return (m.src_and_code & 0x1) != 0;
}

// The 15-bit source block/service ID.
inline uint16_t LogSourceId(const LogMessage &m)
{
    return (uint16_t)((m.src_and_code >> 1) & 0x7FFF);
}

// The 16-bit log code.
inline uint16_t LogCode(const LogMessage &m)
{
    return (uint16_t)(m.src_and_code >> 16);
}

