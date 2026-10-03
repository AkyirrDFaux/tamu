#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "Core/Functions/Crc8.h"
#include "Core/Functions/Align.h" // LoadUnaligned/StoreUnaligned for payload access

// Spec Data Formats.md: Generic packet max 128 total, payload max 116
// Header 12 bytes: CRC8 | Flags | Reserved(4)+Priority(4) | PayloadLen(bytes) | SRC | TGT | CMD | TRID
#ifndef MAX_PAYLOAD_SIZE
#define MAX_PAYLOAD_SIZE 116
#endif

#define MAX_FRAG_CONTENT_SIZE 112

#define FLAG_REQACK (1 << 0)
#define FLAG_START  (1 << 1)
#define FLAG_STOP   (1 << 2)
#define FLAG_TYPE   (1 << 3)
#define FLAG_FRAG   (1 << 4)
#define FLAG_SUCCESS (1 << 5) // response: success, no extra information
#define FLAG_FAIL    (1 << 6) // response: error, no extra information

// Docs/RSBus and Packets.md: the priority byte is Reserved(4) | Priority(4), 0 = highest,
// default 8. The CSMA silence formula is 8 + (priority/8) + random bytes, so the 4-bit
// default of 8 yields a 9-12 byte gap.
#define DEFAULT_PRIORITY 8

// Docs/RSBus and Packets.md "Transaction IDs": every originating service owns a reserved TRID
// range, and a reply echoes the request's TRID - so the range tells the dispatcher where to route
// the response, and each service manages its own allocation (counter or slot table).
#define TRID_SYS_BASE    0x0000
#define TRID_SYS_MAX     0x0FFF // System (Device) + Logs, incrementing
#define TRID_SUB_BASE    0x1000
#define TRID_SUB_MAX     0x1FFF // Subscriptions, table-managed
#define TRID_SCRIPT_BASE 0x2000
#define TRID_SCRIPT_MAX  0x2FFF // Scripts, slot-based
#define TRID_APP_BASE    0xF000
#define TRID_APP_MAX     0xFFFF // App, slot-based

// ID helpers: 6 bit net + 10 bit device
inline uint16_t MakeId(uint8_t net, uint16_t dev) { return (uint16_t)((net & 0x3F) << 10) | (dev & 0x3FF); }

#define ADDR_INVALID   0x0000
#define ADDR_BROADCAST 0xFFFF

// Data Formats.md: 0x3F = broadcast into all nets.
// 3F.1  = all cores (Core discover target), 3F.0 = all unassigned devices.
#define ADDR_ALL_CORES        MakeId(0x3F, 1)

enum class ServiceType : uint8_t
{
    Device = 0x00,
    Register = 0x01,
    LogHandler = 0x02,
    Storage = 0x03,
    Subscriptions = 0x04,
    Script = 0x05,
    App = 0x11
};

// Wire order per Docs/RSBus and Packets.md (top-to-bottom): CRC8 | Flags | Reserved(4) |
// Priority(4) | Payload Length (bytes) | SRC ID | TGT ID | CMD | TRID | Payload.
struct PacketFrame
{
    uint8_t crc8;
    uint8_t flags;
    uint8_t priority;    // Reserved(4) | Priority(4), 0 = highest
    uint8_t payload_len; // payload length in bytes
    uint16_t id_src;                                 // Source device's address
    uint16_t id_tgt;                                 // Target device's address
    union { uint16_t cmd; uint16_t srv_tgt; };       // Command (destination service)
    union { uint16_t trid; uint16_t srv_src; };      // Transaction ID
    uint8_t payload[MAX_PAYLOAD_SIZE];
} __attribute__((packed, aligned(4)));

// `packed` fixes the wire layout but on its own drops the type's alignment to 1, which would
// let a `PacketFrame` local sit anywhere and make every typed payload access misaligned. The
// explicit 4-byte alignment keeps the layout/size identical and makes `payload + 4k` genuinely
// 4-aligned (a u16 at an even offset is fine too).
static_assert(alignof(PacketFrame) == 4, "the payload must be 4-byte aligned for typed access");
static_assert(offsetof(PacketFrame, payload) % 4 == 0, "payload must stay 4-byte aligned");
static_assert(offsetof(PacketFrame, id_src) == 4, "wire order: SRC ID after the length");
static_assert(offsetof(PacketFrame, id_tgt) == 6, "wire order: TGT ID after SRC ID");
static_assert(offsetof(PacketFrame, cmd) == 8, "wire order: CMD after TGT ID");
static_assert(offsetof(PacketFrame, trid) == 10, "wire order: TRID after CMD");

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
    return (uint16_t)frame.payload_len; // bytes (Docs: "Payload Length, in bytes")
}

inline void PacketFinalize(PacketFrame *frame, uint16_t len)
{
    if (len > MAX_PAYLOAD_SIZE) len = MAX_PAYLOAD_SIZE;
    frame->payload_len = (uint8_t)len;
    frame->crc8 = Crc8(&frame->flags, (uint16_t)(11 + len));
}

inline void PacketConstruct(PacketFrame *frame,
                            uint16_t dest_addr,
                            uint16_t dest_cmd,
                            uint16_t trid,
                            uint8_t flags,
                            const uint8_t *payload,
                            uint16_t len,
                            uint8_t priority = DEFAULT_PRIORITY)
{
    if (len > MAX_PAYLOAD_SIZE) len = MAX_PAYLOAD_SIZE;
    frame->flags = flags;
    frame->priority = priority;
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
    reply.cmd = req.srv_src;   // Destination service (the originator's service tag)
    reply.trid = req.trid;     // Responses echo the request's TRID (Docs "Transaction IDs")
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
    // A fragment must carry at least its 4-byte frag info (u16 current + u16 total). A
    // malformed frame that sets FLAG_FRAG with no payload would otherwise read whatever the
    // struct's payload area holds; return an empty range instead. Handlers still bound-check
    // before using the values.
    PacketFragInfo fi;
    if (PayloadBytes(frame) < 4)
    {
        fi.current = 0xFFFF;
        fi.total = 0xFFFF;
        return fi;
    }
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
