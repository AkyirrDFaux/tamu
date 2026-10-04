#pragma once

// Bootloader packet codec (Docs/Services/Bootloader.md).
//
// The bootloader replaces the main binary; it deliberately ignores the standard packet
// format and speaks this raw, byte-packed frame on USB (cores) or RSBus (nodes):
//
//   [0]      0xCA start marker
//   [1]      control: padding(5)=0 | parity(1, even) | command(2)
//   [2..5]   offset, u32 little-endian, relative to the main binary space start
//   [6..37]  32-byte payload  (write and read-response only)
//   [38]     0xBC end marker  (write and read-response)
//
// A read-request is header-only: start | control | offset | end = 7 bytes.
// Commands: 0b01 write, 0b10 read-request, 0b11 read-response.
//
// Parity is even over every bit of the frame EXCEPT the parity bit itself, so a valid frame
// has an even total number of set bits. The doc only says "Even" - this is the strictest
// whole-frame reading; see Issues.md if it should be data-only.

#include <cstdint>
#include <cstring>

namespace Bootloader
{
    constexpr uint8_t START = 0xCA;
    constexpr uint8_t END   = 0xBC;

    constexpr uint8_t CMD_WRITE     = 0x1; // 0b01
    constexpr uint8_t CMD_READ_REQ  = 0x2; // 0b10
    constexpr uint8_t CMD_READ_RESP = 0x3; // 0b11

    constexpr uint8_t PAYLOAD_SIZE   = 32;
    constexpr uint8_t HEADER_SIZE    = 6;  // start + control + offset
    constexpr uint8_t READ_REQ_SIZE  = 7;  // header + end
    constexpr uint8_t DATA_SIZE      = 39; // header + payload + end
    constexpr uint8_t MAX_FRAME_SIZE = DATA_SIZE;

    // Even parity over the whole frame, excluding the parity bit (byte 1, bit 2).
    inline uint8_t Parity(const uint8_t *frame, uint16_t len)
    {
        uint8_t p = 0;
        for (uint16_t i = 0; i < len; i++)
        {
            uint8_t b = frame[i];
            if (i == 1)
                b &= (uint8_t)~0x04u;
            b ^= (uint8_t)(b >> 4);
            b ^= (uint8_t)(b >> 2);
            b ^= (uint8_t)(b >> 1);
            p ^= (uint8_t)(b & 1u);
        }
        return (uint8_t)(p & 1u);
    }

    // Fills byte 1: padding 0, even parity, command. `frame` must already hold the full body.
    inline void WriteControl(uint8_t *frame, uint8_t cmd, uint16_t len)
    {
        frame[1] = (uint8_t)(cmd & 0x03u);
        frame[1] |= (uint8_t)(Parity(frame, len) << 2);
    }

    inline void EncodeWrite(uint32_t offset, const uint8_t *payload, uint8_t *out)
    {
        out[0] = START;
        memcpy(out + 2, &offset, 4);
        memcpy(out + 6, payload, PAYLOAD_SIZE);
        out[DATA_SIZE - 1] = END;
        WriteControl(out, CMD_WRITE, DATA_SIZE);
    }

    inline void EncodeReadRequest(uint32_t offset, uint8_t *out)
    {
        out[0] = START;
        memcpy(out + 2, &offset, 4);
        out[READ_REQ_SIZE - 1] = END;
        WriteControl(out, CMD_READ_REQ, READ_REQ_SIZE);
    }

    inline void EncodeReadResponse(uint32_t offset, const uint8_t *payload, uint8_t *out)
    {
        out[0] = START;
        memcpy(out + 2, &offset, 4);
        memcpy(out + 6, payload, PAYLOAD_SIZE);
        out[DATA_SIZE - 1] = END;
        WriteControl(out, CMD_READ_RESP, DATA_SIZE);
    }

    inline uint32_t Offset(const uint8_t *frame)
    {
        uint32_t o;
        memcpy(&o, frame + 2, 4);
        return o;
    }

    inline const uint8_t *Payload(const uint8_t *frame) { return frame + 6; }

    // The frame length for a command, or 0 when the command is unknown.
    inline uint16_t FrameSize(uint8_t cmd)
    {
        if (cmd == CMD_READ_REQ) return READ_REQ_SIZE;
        if (cmd == CMD_WRITE || cmd == CMD_READ_RESP) return DATA_SIZE;
        return 0;
    }

    // Validates a complete frame; returns the command, or 0 when invalid.
    inline uint8_t Decode(const uint8_t *frame, uint16_t len)
    {
        if (len < READ_REQ_SIZE || frame[0] != START)
            return 0;
        if ((frame[1] & 0xF8u) != 0) // padding must be zero
            return 0;
        uint8_t cmd = (uint8_t)(frame[1] & 0x03u);
        uint16_t expect = FrameSize(cmd);
        if (expect == 0 || len != expect || frame[len - 1] != END)
            return 0;
        uint8_t got = (uint8_t)((frame[1] >> 2) & 1u);
        return (got == Parity(frame, len)) ? cmd : 0;
    }
}
