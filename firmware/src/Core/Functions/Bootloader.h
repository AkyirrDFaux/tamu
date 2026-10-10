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
// Parity is even over the "rest of the packet" after the control byte's padding/parity: the
// command (2 bits), the offset (32 bits) and the payload (32 bytes, when present). The start
// marker, the 5 padding bits and the end marker are excluded.

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

    // Even parity over the command (2 bits), the offset and the payload (bytes 2..len-2).
    inline uint8_t Parity(const uint8_t *Frame, uint16_t Len)
    {
        uint8_t p = 0;
        uint8_t Cmd = (uint8_t)(Frame[1] & 0x03u);
        p ^= (uint8_t)(Cmd & 1u) ^ (uint8_t)((Cmd >> 1) & 1u);
        for (uint16_t i = 2; i + 1 < Len; i++)
        {
            uint8_t b = Frame[i];
            b ^= (uint8_t)(b >> 4);
            b ^= (uint8_t)(b >> 2);
            b ^= (uint8_t)(b >> 1);
            p ^= (uint8_t)(b & 1u);
        }
        return (uint8_t)(p & 1u);
    }

    // Fills byte 1: padding 0, even parity, command. `frame` must already hold the full body.
    inline void WriteControl(uint8_t *Frame, uint8_t Cmd, uint16_t Len)
    {
        Frame[1] = (uint8_t)(Cmd & 0x03u);
        Frame[1] |= (uint8_t)(Parity(Frame, Len) << 2);
    }

    inline void EncodeWrite(uint32_t Offset, const uint8_t *Payload, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        memcpy(Out + 6, Payload, PAYLOAD_SIZE);
        Out[DATA_SIZE - 1] = END;
        WriteControl(Out, CMD_WRITE, DATA_SIZE);
    }

    inline void EncodeReadRequest(uint32_t Offset, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        Out[READ_REQ_SIZE - 1] = END;
        WriteControl(Out, CMD_READ_REQ, READ_REQ_SIZE);
    }

    inline void EncodeReadResponse(uint32_t Offset, const uint8_t *Payload, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        memcpy(Out + 6, Payload, PAYLOAD_SIZE);
        Out[DATA_SIZE - 1] = END;
        WriteControl(Out, CMD_READ_RESP, DATA_SIZE);
    }

    inline uint32_t Offset(const uint8_t *Frame)
    {
        uint32_t o;
        memcpy(&o, Frame + 2, 4);
        return o;
    }

    inline const uint8_t *Payload(const uint8_t *Frame) { return Frame + 6; }

    // The frame length for a command, or 0 when the command is unknown.
    inline uint16_t FrameSize(uint8_t Cmd)
    {
        if (Cmd == CMD_READ_REQ) return READ_REQ_SIZE;
        if (Cmd == CMD_WRITE || Cmd == CMD_READ_RESP) return DATA_SIZE;
        return 0;
    }

    // Validates a complete frame; returns the command, or 0 when invalid.
    inline uint8_t Decode(const uint8_t *Frame, uint16_t Len)
    {
        if (Len < READ_REQ_SIZE || Frame[0] != START)
            return 0;
        if ((Frame[1] & 0xF8u) != 0) // padding must be zero
            return 0;
        uint8_t Cmd = (uint8_t)(Frame[1] & 0x03u);
        uint16_t Expect = FrameSize(Cmd);
        if (Expect == 0 || Len != Expect || Frame[Len - 1] != END)
            return 0;
        uint8_t Got = (uint8_t)((Frame[1] >> 2) & 1u);
        return (Got == Parity(Frame, Len)) ? Cmd : 0;
    }
}
