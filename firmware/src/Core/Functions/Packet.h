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

// Word-stream senders (Register enumerate, the Script slot map, the Storage read stream) pack
// whole u16 words into one fragment: 32 words = 64 content bytes, well inside
// MAX_FRAG_CONTENT_SIZE. Defined once so the senders cannot drift apart.
#define MAX_FRAG_WORDS 32
#define MAX_FRAG_WORD_BYTES (MAX_FRAG_WORDS * 2)

#define FLAG_REQACK (1 << 0)
#define FLAG_START  (1 << 1)
#define FLAG_STOP   (1 << 2)
#define FLAG_TYPE   (1 << 3)
#define FLAG_FRAG   (1 << 4)
#define FLAG_SUCCESS (1 << 5) // response: success; an optional payload after it carries detail
#define FLAG_FAIL    (1 << 6) // response: failure; an optional payload after it carries a reason

// Docs/RSBus and Packets.md: the priority byte is Reserved(4) | Priority(4), 0 = highest,
// default 8. The CSMA silence formula is 8 + (priority/8) + random bytes, so the 4-bit
// default of 8 yields a 9-12 byte gap.
#define DEFAULT_PRIORITY 8

// The documented priority classes (Docs/RSBus and Packets.md "Priorities"), highest first.
// The subscription values are the ones already carried by the subscription entries.
#define PRIORITY_ERROR    0  // Errors (highest)
#define PRIORITY_TIMESYNC 1  // TimeSync packets
#define PRIORITY_SUB_HIGH 4  // High-priority subscriptions
//                                     8 = DEFAULT_PRIORITY ("Other")
#define PRIORITY_SUB_LOW  12 // Low-priority subscriptions
#define PRIORITY_STREAM   13 // Streams
#define PRIORITY_LOG      15 // Logs (lowest)

// Docs/RSBus and Packets.md "Transaction IDs": each originating service owns a reserved TRID
// range, and a reply echoes the request's TRID - so the range tells the dispatcher where to route
// the response, and each service manages its own allocation (counter or slot table).
#define TRID_SYSTEM_MAX  0x0FFF // System/Logs, single incrementing counter (0x0000-0x0FFF)
#define TRID_SUB_BASE    0x1000
#define TRID_SUB_MAX     0x1FFF // Subscriptions, table-managed
#define TRID_SCRIPT_BASE 0x2000
#define TRID_SCRIPT_MAX  0xEFFF // Scripts, slot-based (0x2000-0xEFFF)
#define TRID_APP_BASE    0xF000 // App, slot-based (0xF000-0xFFFF)

// ID helpers: 6 bit net + 10 bit device
inline uint16_t MakeId(uint8_t Net, uint16_t Dev) { return (uint16_t)((Net & 0x3F) << 10) | (Dev & 0x3FF); }

#define ADDR_INVALID   0x0000
#define ADDR_BROADCAST 0xFFFF

// Data Formats.md: 0x3F = broadcast into all nets.
// 3F.1  = all cores (Core discover target), 3F.0 = all unassigned devices.
#define ADDR_ALL_CORES        MakeId(0x3F, 1)

// Net 0 means "the local net": when matching or building an address, a net-0 target resolves to
// the local net number. Broadcasts (net 0x3F) and already net-qualified addresses are unchanged.
inline uint16_t NetQualifyLocal(uint16_t Addr, uint8_t LocalNet)
{
    if ((Addr >> 10) == 0) return MakeId(LocalNet, Addr & 0x3FF);
    return Addr;
}

enum class ServiceType : uint8_t
{
    Device = 0x00,
    Register = 0x01,
    LogHandler = 0x02,
    Storage = 0x03,
    Subscriptions = 0x04,
    Script = 0x05
};

// Wire order per Docs/RSBus and Packets.md (top-to-bottom): CRC8 | Flags | Reserved(4) |
// Priority(4) | Payload Length (bytes) | SRC ID | TGT ID | CMD | TRID | Payload.
struct PacketFrame
{
    uint8_t Crc8;
    uint8_t Flags;
    uint8_t Priority;    // Reserved(4) | Priority(4), 0 = highest
    uint8_t PayloadLen; // payload length in bytes
    uint16_t IdSrc;                                 // Source device's address
    uint16_t IdTgt;                                 // Target device's address
    uint16_t Cmd;                                    // Command ID
    uint16_t Trid;                                   // Transaction ID
    uint8_t Payload[MAX_PAYLOAD_SIZE];
} __attribute__((packed, aligned(4)));

// `packed` fixes the wire layout but on its own drops the type's alignment to 1, which would
// let a `PacketFrame` local sit anywhere and make every typed payload access misaligned. The
// explicit 4-byte alignment keeps the layout/size identical and makes `payload + 4k` genuinely
// 4-aligned (a u16 at an even offset is fine too).
static_assert(alignof(PacketFrame) == 4, "the Payload must be 4-byte aligned for typed access");
static_assert(offsetof(PacketFrame, Payload) % 4 == 0, "Payload must stay 4-byte aligned");
static_assert(offsetof(PacketFrame, IdSrc) == 4, "wire order: SRC ID after the length");
static_assert(offsetof(PacketFrame, IdTgt) == 6, "wire order: TGT ID after SRC ID");
static_assert(offsetof(PacketFrame, Cmd) == 8, "wire order: CMD after TGT ID");
static_assert(offsetof(PacketFrame, Trid) == 10, "wire order: TRID after CMD");

inline uint16_t MakeService(ServiceType Type, uint8_t Cid)
{
    return ((uint16_t)Type << 8) | Cid;
}
inline ServiceType GetServiceType(uint16_t Cmd)
{
    return (ServiceType)(Cmd >> 8);
}
inline uint8_t GetServiceCID(uint16_t Cmd)
{
    return (uint8_t)(Cmd & 0xFF);
}

// System/Log TRIDs: a single incrementing 12-bit counter (Docs "Transaction IDs",
// 0x0000-0x0FFF), shared by every service and wrapping at the range end. The service is not
// encoded here: a reply carries the request's CMD (which names the service) and echoes the
// TRID (see FinalizeReply), so a reply routes by its CMD and its TRID is a plain match key.
// The `type` argument is retained only so existing call sites need not change.
inline uint16_t NextSystemTrid(ServiceType /*type*/)
{
    static uint16_t Counter = 0;
    const uint16_t Trid = Counter;
    Counter = (uint16_t)((Counter + 1) & TRID_SYSTEM_MAX);
    return Trid;
}

extern DeviceStatusStruct DeviceStatus;

inline uint16_t PayloadBytes(const PacketFrame &Frame)
{
    return (uint16_t)Frame.PayloadLen; // bytes (Docs: "Payload Length, in bytes")
}

inline void PacketFinalize(PacketFrame *Frame, uint16_t Len)
{
    if (Len > MAX_PAYLOAD_SIZE) Len = MAX_PAYLOAD_SIZE;
    Frame->PayloadLen = (uint8_t)Len;
    Frame->Crc8 = Crc8(&Frame->Flags, (uint16_t)(11 + Len));
}

inline void PacketConstruct(PacketFrame *Frame,
                            uint16_t DestAddr,
                            uint16_t DestCmd,
                            uint16_t Trid,
                            uint8_t Flags,
                            const uint8_t *Payload,
                            uint16_t Len,
                            uint8_t Priority = DEFAULT_PRIORITY)
{
    if (Len > MAX_PAYLOAD_SIZE) Len = MAX_PAYLOAD_SIZE;
    Frame->Flags = Flags;
    Frame->Priority = Priority;
    Frame->IdTgt = DestAddr;
    Frame->IdSrc = DeviceStatus.ShortAddress;
    Frame->Cmd = DestCmd;
    Frame->Trid = Trid;
    if (Payload && Len > 0) memcpy(Frame->Payload, Payload, Len);
    PacketFinalize(Frame, Len);
}

// Finalises a fragmented reply. Every caller streams data, so the default is the Streams class;
// the log database stream overrides it with the Logs class.
inline void FinalizeReply(PacketFrame &Reply, const PacketFrame &Req, uint8_t Flags, uint16_t Len,
                          uint8_t Priority = PRIORITY_STREAM)
{
    Reply.Flags = Flags;
    Reply.Priority = Priority;
    Reply.IdTgt = Req.IdSrc;
    Reply.IdSrc = DeviceStatus.ShortAddress;
    Reply.Cmd = Req.Cmd;   // Destination service: the request's CMD names the service
    Reply.Trid = Req.Trid;     // Responses echo the request's TRID (Docs "Transaction IDs")
    PacketFinalize(&Reply, Len);
}

inline void WriteFragInfo(uint8_t *Out, uint16_t Current, uint16_t Total)
{
    Out[0] = (uint8_t)Current;
    Out[1] = (uint8_t)(Current >> 8);
    Out[2] = (uint8_t)Total;
    Out[3] = (uint8_t)(Total >> 8);
}

struct PacketFragInfo
{
    uint16_t Current;
    uint16_t Total;
};

inline PacketFragInfo PacketGetFrag(const PacketFrame &Frame)
{
    // A fragment must carry at least its 4-byte frag info (u16 current + u16 total). A
    // malformed frame that sets FLAG_FRAG with no payload would otherwise read whatever the
    // struct's payload area holds; return an empty range instead. Handlers still bound-check
    // before using the values.
    PacketFragInfo Fi;
    if (PayloadBytes(Frame) < 4)
    {
        Fi.Current = 0xFFFF;
        Fi.Total = 0xFFFF;
        return Fi;
    }
    Fi.Current = (uint16_t)(Frame.Payload[0] | (Frame.Payload[1] << 8));
    Fi.Total = (uint16_t)(Frame.Payload[2] | (Frame.Payload[3] << 8));
    return Fi;
}

inline uint16_t PacketWireSize(const PacketFrame *Frame)
{
    return (uint16_t)(12 + PayloadBytes(*Frame));
}
