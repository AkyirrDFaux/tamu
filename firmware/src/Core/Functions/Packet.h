#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// Spec Data Formats.md: Generic packet max 128 total, payload max 116
// Header 12 bytes: CRC8 | Flags | Priority | PayloadLen | TGT | SRC | CMD | TRID
#ifndef MAX_PAYLOAD_SIZE
#define MAX_PAYLOAD_SIZE 116
#endif

#define MAX_FRAG_CONTENT_SIZE 112

#define FLAG_REQACK (1 << 0)
#define FLAG_START  (1 << 1)
#define FLAG_STOP   (1 << 2)
#define FLAG_TYPE   (1 << 3)
#define FLAG_FRAG   (1 << 4)

// Docs/RSBus and Packets.md: the priority byte is Reserved(4) | Priority(4), 0 = highest,
// default 8. The CSMA silence formula is 8 + (priority/8) + random bytes, so the 4-bit
// default of 8 yields a 9-12 byte gap.
#define DEFAULT_PRIORITY 8

// ID helpers: 6 bit net + 10 bit device
inline uint16_t MakeId(uint8_t net, uint16_t dev) { return (uint16_t)((net & 0x3F) << 10) | (dev & 0x3FF); }
inline uint8_t IdNet(uint16_t id) { return (uint8_t)((id >> 10) & 0x3F); }
inline uint16_t IdDev(uint16_t id) { return id & 0x3FF; }

#define ADDR_INVALID   0x0000
#define ADDR_BROADCAST 0xFFFF
#define ADDR_BRANCH_BROADCAST 0xFFFE

// Data Formats.md: 0x3F = broadcast into all nets.
// 3F.1  = all cores (Core discover target), 3F.0 = all unassigned devices.
#define ADDR_ALL_CORES        MakeId(0x3F, 1)
#define ADDR_ALL_UNASSIGNED   MakeId(0x3F, 0)

enum class ServiceType : uint8_t
{
    Device = 0x00,
    Register = 0x01,
    LogHandler = 0x02,
    Storage = 0x03,
    Subscriptions = 0x04,
    Router = 0x10,
    App = 0x11,
    CLI = 0x12
};

struct PacketFrame
{
    uint8_t crc8;
    uint8_t flags;
    uint8_t priority;
    uint8_t payload_len;
    uint16_t id_tgt;
    uint16_t id_src;
    union { uint16_t cmd; uint16_t srv_tgt; };
    union { uint16_t trid; uint16_t srv_src; };
    uint8_t payload[MAX_PAYLOAD_SIZE];
} __attribute__((packed));

static_assert(offsetof(PacketFrame, payload) % 4 == 0, "payload must stay 4-byte aligned");

inline uint8_t Crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07;
            else crc <<= 1;
        }
    }
    return crc;
}

inline uint16_t MakeService(ServiceType type, uint8_t cid)
{
    return ((uint16_t)type << 8) | cid;
}
inline ServiceType GetServiceType(uint16_t cmd)
{
    return (ServiceType)(cmd >> 8);
}
inline uint8_t GetServiceCID(uint16_t cmd)
{
    return (uint8_t)(cmd & 0xFF);
}

extern DeviceStatusStruct DeviceStatus;

inline uint16_t PayloadBytes(const PacketFrame &frame)
{
    return (uint16_t)frame.payload_len * 4;
}

inline void PacketFinalize(PacketFrame *frame, uint16_t len)
{
    uint16_t padded = (uint16_t)((len + 3u) & ~3u);
    if (padded > MAX_PAYLOAD_SIZE) padded = MAX_PAYLOAD_SIZE;
    if (padded > len) memset(frame->payload + len, 0, padded - len);
    frame->payload_len = (uint8_t)(padded / 4);
    frame->crc8 = Crc8(&frame->flags, (uint16_t)(11 + padded));
}

inline void PacketConstruct(PacketFrame *frame,
                            uint16_t dest_addr,
                            uint16_t dest_cmd,
                            uint16_t trid,
                            uint8_t flags,
                            const uint8_t *payload,
                            uint16_t len)
{
    if (len > MAX_PAYLOAD_SIZE) len = MAX_PAYLOAD_SIZE;
    frame->flags = flags;
    frame->priority = DEFAULT_PRIORITY;
    frame->id_tgt = dest_addr;
    frame->id_src = DeviceStatus.ShortAddress;
    frame->cmd = dest_cmd;
    frame->trid = trid;
    if (payload && len > 0) memcpy(frame->payload, payload, len);
    PacketFinalize(frame, len);
}

inline void FinalizeReply(PacketFrame &reply, const PacketFrame &req, uint8_t flags, uint16_t len)
{
    reply.flags = flags;
    reply.priority = DEFAULT_PRIORITY;
    reply.id_tgt = req.id_src;
    reply.id_src = DeviceStatus.ShortAddress;
    reply.cmd = req.srv_src;   // Destination service (was request's source)
    reply.trid = req.srv_tgt;  // Source service (was request's target)
    PacketFinalize(&reply, len);
}

inline void WriteFragInfo(uint8_t *out, uint16_t current, uint16_t total)
{
    out[0] = (uint8_t)current;
    out[1] = (uint8_t)(current >> 8);
    out[2] = (uint8_t)total;
    out[3] = (uint8_t)(total >> 8);
}

struct PacketFragInfo
{
    uint16_t current;
    uint16_t total;
};

inline PacketFragInfo PacketGetFrag(const PacketFrame &frame)
{
    PacketFragInfo fi;
    fi.current = (uint16_t)(frame.payload[0] | (frame.payload[1] << 8));
    fi.total = (uint16_t)(frame.payload[2] | (frame.payload[3] << 8));
    return fi;
}

inline uint16_t PacketWireSize(const PacketFrame *frame)
{
    return (uint16_t)(12 + PayloadBytes(*frame));
}

inline void PacketToWire(const PacketFrame *frame, uint8_t *out)
{
    memcpy(out, frame, PacketWireSize(frame));
}
