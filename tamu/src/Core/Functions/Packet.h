#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

#define MAX_PAYLOAD_SIZE 256

// Flag bitmasks (from Data Formats.md)
#define FLAG_REQACK (1 << 0)
#define FLAG_START  (1 << 1)
#define FLAG_STOP   (1 << 2)
#define FLAG_TYPE   (1 << 3) // 0 = Request, 1 = Response

// Address constants
#define ADDR_INVALID   0x0000
#define ADDR_BROADCAST 0xFFFF

// Service Types (from General architecture.md)
enum class ServiceType : uint8_t
{
    Device = 0x01,
    LogHandler = 0x02,
    Storage = 0x03,
    SystemMemory = 0x04,
    DynamicMemory = 0x05,
    KeyedMemory = 0x06,
    Script = 0x07,
    CLI = 0x09
};

// Packets structure
struct PacketFrame
{
    uint8_t crc8;
    uint8_t flags;
    uint8_t frag_id;
    uint8_t payload_len;
    uint16_t id_tgt;
    uint16_t id_src;
    uint16_t srv_tgt; // (8bit service type + 8bit custom identifier)
    uint16_t srv_src; // (8bit service type + 8bit custom identifier)
    uint8_t payload[MAX_PAYLOAD_SIZE];
} __attribute__((packed));

// Direct word/halfword reads from frame.payload (e.g. payload[0], payload+8 as uint32_t)
// rely on this alignment; RV32EC would fault on a misaligned load. If the header layout
// ever changes, convert those call sites to memcpy instead.
static_assert(offsetof(PacketFrame, payload) % 4 == 0,
              "payload must stay 4-byte aligned for direct word reads");

// Computes the CRC8 checksum over `len` bytes of `data` (polynomial 0x07, init 0x00).
// The length is 16-bit: frames carry up to 11 + 255 = 266 covered bytes, which a
// uint8_t would truncate mod 256.
inline uint8_t Crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

// Packs a ServiceType and custom identifier into the 16-bit srv field
inline uint16_t MakeService(ServiceType type, uint8_t cid)
{
    return ((uint16_t)type << 8) | cid;
}

// Extracts the ServiceType (high byte) from a packed srv field
inline ServiceType GetServiceType(uint16_t srv)
{
    return (ServiceType)(srv >> 8);
}

// Extracts the custom identifier (low byte) from a packed srv field
inline uint8_t GetServiceCID(uint16_t srv)
{
    return (uint8_t)(srv & 0xFF);
}

// Global reference for device status (struct defined in Blocks/DeviceInfo.h)
extern DeviceStatusStruct DeviceStatus;

// Builds a packet frame with header fields filled and CRC8 computed over the header + payload
inline void PacketConstruct(PacketFrame *frame,
                             uint16_t dest_addr,
                             uint16_t dest_srv,
                             uint16_t src_srv,
                             uint8_t flags,
                             const uint8_t *payload,
                             uint8_t len)
{
    // payload_len is a single wire byte, so more than 255 payload bytes cannot be
    // represented; clamp rather than silently truncating to 0.
    if (len > MAX_PAYLOAD_SIZE - 1)
        len = MAX_PAYLOAD_SIZE - 1;

    // Clear only the header: the payload beyond payload_len is never read (the CRC
    // covers 11 + payload_len bytes), so a full ~264 B memset is wasted work.
    frame->flags = flags;
    frame->frag_id = 0;
    frame->payload_len = len;
    frame->id_tgt = dest_addr;
    frame->id_src = DeviceStatus.ShortAddress;
    frame->srv_tgt = dest_srv;
    frame->srv_src = src_srv;

    if (payload && frame->payload_len > 0)
    {
        memcpy(frame->payload, payload, frame->payload_len);
    }
    
    // CRC8 calculation covers everything after the crc8 field
    uint16_t crc_len = 11 + frame->payload_len; // flags, frag_id, payload_len, target/source IDs/SRVs
    frame->crc8 = Crc8(&frame->flags, crc_len);
}

// Returns the sequential FragID for a multi-packet stream (Data Formats.md: "FragID -
// Sequential number, 0 default"). Resets to 0 for the START packet and increments for each
// following packet, so every packet of a stream carries a monotonic fragment number. Single
// packets (START|STOP) always get 0. The counter is only kept on the core, which is the main
// sender of multi-packet streams (topology/registry/file/backup dumps); RAM-starved nodes
// keep 0 and only ever reply with single packets.
inline uint8_t NextFragmentId(uint8_t flags)
{
#ifdef TYPE_CORE
    static uint8_t frag_id = 0;
    if (flags & FLAG_START)
        frag_id = 0;
    else
        frag_id++;
    return frag_id;
#else
    return 0;
#endif
}

// Appends `len` bytes of `data` to the frame payload (returns false if it would overflow) and recomputes CRC8
inline bool PacketAppend(PacketFrame *frame, const uint8_t *data, uint8_t len)
{
    // payload_len is a uint8_t, so the wire cannot carry more than 255 payload bytes
    // (MAX_PAYLOAD_SIZE is only the buffer size).
    if ((frame->payload_len + len) > (MAX_PAYLOAD_SIZE - 1))
    {
        return false;
    }

    memcpy(&frame->payload[frame->payload_len], data, len);
    frame->payload_len += len;
    
    uint16_t crc_len = 11 + frame->payload_len;
    frame->crc8 = Crc8(&frame->flags, crc_len);
    return true;
}

