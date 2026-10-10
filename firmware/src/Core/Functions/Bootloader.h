#pragma once

// Bootloader packet codec (Docs/Services/Bootloader.md).
//
// The bootloader replaces the main binary; it deliberately ignores the standard packet
// format and speaks this raw, byte-packed frame on USB (cores) or RSBus (nodes):
//
//   [0]      0xCA start marker
//   [1]      control: padding(4)=0 | parity(1, even) | command(3)
//   [2..5]   offset, u32 little-endian, relative to the main binary space start
//   [6..37]  32-byte payload  (write, read-response and identify-response)
//   [38]     0xBC end marker  (write, read-response and identify-response)
//
// Header-only frames (no payload): start | control | offset | end = 7 bytes.
// Commands: 0b000 identify-request, 0b001 identify-response, 0b010 write-request,
//           0b011 write-response, 0b100 read-request, 0b101 read-response.
//
// The identify-response carries the app-space size in the offset slot (u32 LE) and the
// device name as the 32-byte payload (null-padded); the write-response carries the offset
// of the last successfully written chunk in the offset slot. Both are header-only.
//
// Parity is even over the "rest of the packet" after the control byte's padding/parity: the
// command (3 bits), the offset (32 bits) and the payload (32 bytes, when present). The start
// marker, the 4 padding bits and the end marker are excluded.

#include <cstdint>
#include <cstring>

namespace Bootloader
{
    constexpr uint8_t START = 0xCA;
    constexpr uint8_t END   = 0xBC;

    constexpr uint8_t CMD_IDENTIFY_REQ  = 0x0; // 0b000
    constexpr uint8_t CMD_IDENTIFY_RESP = 0x1; // 0b001
    constexpr uint8_t CMD_WRITE         = 0x2; // 0b010
    constexpr uint8_t CMD_WRITE_RESP    = 0x3; // 0b011
    constexpr uint8_t CMD_READ_REQ      = 0x4; // 0b100
    constexpr uint8_t CMD_READ_RESP     = 0x5; // 0b101

    // Decode sentinel: 0 is a valid command (identify-request), so "invalid" is 0xFF.
    constexpr uint8_t INVALID = 0xFF;

    constexpr uint8_t PAYLOAD_SIZE   = 32;
    constexpr uint8_t HEADER_SIZE    = 6;  // start + control + offset
    constexpr uint8_t READ_REQ_SIZE  = 7;  // header + end
    constexpr uint8_t DATA_SIZE      = 39; // header + payload + end
    constexpr uint8_t NAME_SIZE      = 32; // identify-response name (null-padded)
    constexpr uint8_t MAX_FRAME_SIZE = DATA_SIZE;

    // Even parity over the command (3 bits), the offset and the payload (bytes 2..len-2).
    constexpr uint8_t Parity(const uint8_t *Frame, uint16_t Len)
    {
        uint8_t Parity = 0;
        uint8_t Cmd = (uint8_t)(Frame[1] & 0x07u);
        Parity ^= (uint8_t)(Cmd & 1u) ^ (uint8_t)((Cmd >> 1) & 1u) ^ (uint8_t)((Cmd >> 2) & 1u);
        for (uint16_t i = 2; i + 1 < Len; i++)
        {
            uint8_t Byte = Frame[i];
            Byte ^= (uint8_t)(Byte >> 4);
            Byte ^= (uint8_t)(Byte >> 2);
            Byte ^= (uint8_t)(Byte >> 1);
            Parity ^= (uint8_t)(Byte & 1u);
        }
        return (uint8_t)(Parity & 1u);
    }

    // Fills byte 1: padding 0, even parity, command. `Frame` must already hold the full body.
    inline void WriteControl(uint8_t *Frame, uint8_t Cmd, uint16_t Len)
    {
        Frame[1] = (uint8_t)(Cmd & 0x07u);
        Frame[1] |= (uint8_t)(Parity(Frame, Len) << 3);
    }

    inline void EncodeWrite(uint32_t Offset, const uint8_t *Payload, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        memcpy(Out + 6, Payload, PAYLOAD_SIZE);
        Out[DATA_SIZE - 1] = END;
        WriteControl(Out, CMD_WRITE, DATA_SIZE);
    }

    // Acknowledges a write: header-only, carrying the offset of the last successfully written chunk.
    inline void EncodeWriteResponse(uint32_t Offset, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        Out[READ_REQ_SIZE - 1] = END;
        WriteControl(Out, CMD_WRITE_RESP, READ_REQ_SIZE);
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

    // Identify request: header-only, offset slot zero.
    inline void EncodeIdentifyRequest(uint8_t *Out)
    {
        uint32_t Zero = 0;
        Out[0] = START;
        memcpy(Out + 2, &Zero, 4);
        Out[READ_REQ_SIZE - 1] = END;
        WriteControl(Out, CMD_IDENTIFY_REQ, READ_REQ_SIZE);
    }

    // Identify response: app-space size (u32) in the offset slot, name as the 32-byte payload.
    inline void EncodeIdentifyResponse(uint32_t AppSize, const char *Name, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &AppSize, 4);
        memset(Out + 6, 0, NAME_SIZE);
        uint8_t i = 0;
        while (i < NAME_SIZE && Name[i] != '\0')
        {
            Out[6 + i] = (uint8_t)Name[i];
            i++;
        }
        Out[DATA_SIZE - 1] = END;
        WriteControl(Out, CMD_IDENTIFY_RESP, DATA_SIZE);
    }

    inline uint32_t Offset(const uint8_t *Frame)
    {
        uint32_t Offset;
        memcpy(&Offset, Frame + 2, 4);
        return Offset;
    }

    inline const uint8_t *Payload(const uint8_t *Frame) { return Frame + 6; }

    // The frame length for a command, or 0 when the command is unknown.
    inline uint16_t FrameSize(uint8_t Cmd)
    {
        if (Cmd == CMD_IDENTIFY_REQ || Cmd == CMD_READ_REQ || Cmd == CMD_WRITE_RESP)
            return READ_REQ_SIZE;
        if (Cmd == CMD_IDENTIFY_RESP || Cmd == CMD_WRITE || Cmd == CMD_READ_RESP)
            return DATA_SIZE;
        return 0;
    }

    // Validates a complete frame; returns the command, or INVALID when the frame is bad.
    inline uint8_t Decode(const uint8_t *Frame, uint16_t Len)
    {
        if (Len < READ_REQ_SIZE || Frame[0] != START)
            return INVALID;
        if ((Frame[1] & 0xF0u) != 0) // padding must be zero
            return INVALID;
        uint8_t Cmd = (uint8_t)(Frame[1] & 0x07u);
        uint16_t Expect = FrameSize(Cmd);
        if (Expect == 0 || Len != Expect || Frame[Len - 1] != END)
            return INVALID;
        uint8_t Got = (uint8_t)((Frame[1] >> 3) & 1u);
        return (Got == Parity(Frame, Len)) ? Cmd : INVALID;
    }
}
