#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// Max payload = max Information (20) + max Actual payload (256) = 276 bytes,
// frame total = 12 header + 276 = 288 (Data Formats.md: "all devices have to handle
// it in full"). Every node builds with this exact value - nodes must not reduce it.
#ifndef MAX_PAYLOAD_SIZE
#define MAX_PAYLOAD_SIZE 276
#endif

// Maximum actual payload bytes per FRAG fragment (Data Formats.md: "Actual payload max 256 bytes").
// The 4-byte fragmentation info goes in the Information section; remaining payload capacity is larger
// but the spec caps actual payload at 256 to ensure all implementations can handle it.
#define MAX_FRAG_CONTENT_SIZE 256

// Flag bitmasks (from Data Formats.md)
#define FLAG_REQACK (1 << 0)
#define FLAG_START  (1 << 1)
#define FLAG_STOP   (1 << 2)
#define FLAG_TYPE   (1 << 3) // 0 = Request, 1 = Response
#define FLAG_FRAG   (1 << 4) // first 4 payload bytes = fragmentation info (u16 current + u16 total)

// Priority byte: 0 = highest, default 128 (Data Formats.md).
#define DEFAULT_PRIORITY 128

// Address constants
#define ADDR_INVALID   0x0000
#define ADDR_BROADCAST 0xFFFF

// Service Types (from Docs/Service ID table.md)
enum class ServiceType : uint8_t
{
    Device = 0x00,
    LogHandler = 0x01,
    Storage = 0x02,
    SystemMemory = 0x04,
    DynamicMemory = 0x05,
    KeyedMemory = 0x06,
    Script = 0x08,
    ScriptInstructions = 0x09,
    Router = 0x10,
    App = 0x11, // App Interface: the app's identity is this service type (SRV SRC high byte),
                // the CID byte is an app-managed transaction ID. No dedicated network address.
    CLI = 0x12,
    Bootloader = 0x13
};

// Packets structure. payload_len holds the WIRE value (in 4-byte units, Data
// Formats.md "in multiples of 4 bytes"); use PayloadBytes() for the byte count.
struct PacketFrame
{
    uint8_t crc8;
    uint8_t flags;
    uint8_t priority;
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
// The length is 16-bit: frames carry up to 11 + 276 = 287 covered bytes, which a
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

// Byte count of a frame's payload. payload_len is stored as the wire value (4-byte
// units); every consumer that needs a byte length must go through this helper - the
// wire length only ever describes 4-byte multiples (payloads are padded to 4).
inline uint16_t PayloadBytes(const PacketFrame &frame)
{
    return (uint16_t)frame.payload_len * 4;
}

// Shared helper: pad `len` bytes already in `frame->payload` to 4, set payload_len and CRC.
inline void PacketFinalize(PacketFrame *frame, uint16_t len)
{
    uint16_t padded = (uint16_t)((len + 3u) & ~3u);
    if (padded > len) memset(frame->payload + len, 0, padded - len);
    frame->payload_len = (uint8_t)(padded / 4);
    frame->crc8 = Crc8(&frame->flags, (uint16_t)(11 + padded));
}

// Builds a packet frame with header fields filled and CRC8 computed over the header + payload.
// The payload is zero-padded to a multiple of 4 and payload_len stores the padded size in
// 4-byte units (Data Formats.md: Payload Length in multiples of 4, max 69 units).
inline void PacketConstruct(PacketFrame *frame,
                            uint16_t dest_addr,
                            uint16_t dest_srv,
                            uint16_t src_srv,
                            uint8_t flags,
                            const uint8_t *payload,
                            uint16_t len)
{
    if (len > MAX_PAYLOAD_SIZE)
        len = MAX_PAYLOAD_SIZE;
    frame->flags = flags;
    frame->priority = DEFAULT_PRIORITY;
    frame->id_tgt = dest_addr;
    frame->id_src = DeviceStatus.ShortAddress;
    frame->srv_tgt = dest_srv;
    frame->srv_src = src_srv;
    if (payload && len > 0)
        memcpy(frame->payload, payload, len);
    PacketFinalize(frame, len);
}

// Finalises a response whose contents were packed directly into `reply.payload`
// (avoids a second scratch buffer, keeping RAM-starved nodes' stacks shallow):
// fills the header from the request, pads the payload to 4 and computes the CRC.
inline void FinalizeReply(PacketFrame &reply, const PacketFrame &req,
                          uint8_t flags, uint16_t len)
{
    reply.flags = flags;
    reply.priority = DEFAULT_PRIORITY;
    reply.id_tgt = req.id_src;
    reply.id_src = DeviceStatus.ShortAddress;
    reply.srv_tgt = req.srv_src;
    reply.srv_src = req.srv_tgt;
    PacketFinalize(&reply, len);
}

// Writes the 4-byte fragmentation info (u16 current fragment + u16 total fragments)
// at `out` (Data Formats.md: FRAG = first 4 payload bytes).
inline void WriteFragInfo(uint8_t *out, uint16_t current, uint16_t total)
{
    out[0] = (uint8_t)current;
    out[1] = (uint8_t)(current >> 8);
    out[2] = (uint8_t)total;
    out[3] = (uint8_t)(total >> 8);
}

// Fragmentation info parsed from the first 4 payload bytes of a FRAG-flagged frame.
struct PacketFragInfo
{
    uint16_t current;
    uint16_t total;
};

// Reads the fragmentation info from the first 4 payload bytes of a FRAG-flagged frame.
inline PacketFragInfo PacketGetFrag(const PacketFrame &frame)
{
    PacketFragInfo fi;
    fi.current = (uint16_t)(frame.payload[0] | (frame.payload[1] << 8));
    fi.total = (uint16_t)(frame.payload[2] | (frame.payload[3] << 8));
    return fi;
}

// On-wire size of a frame: 12 header bytes (crc8 + flags + priority + payload_len +
// 2x id + 2x srv) followed by PayloadBytes() payload bytes. The trailing unused bytes
// of the PacketFrame struct are NOT transmitted.
inline uint16_t PacketWireSize(const PacketFrame *frame)
{
    return (uint16_t)(12 + PayloadBytes(*frame));
}

// Serializes a frame into `out` (must hold PacketWireSize(frame) bytes). The layout
// equals the RSBus wire format minus the leading sync byte: crc8 first, then the raw
// struct bytes (payload_len already holds the wire value in units). The app's
// PacketStreamParser expects exactly this layout.
inline void PacketToWire(const PacketFrame *frame, uint8_t *out)
{
    memcpy(out, frame, PacketWireSize(frame));
}