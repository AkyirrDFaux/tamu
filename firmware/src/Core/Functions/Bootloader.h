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
//           0b011 write-response, 0b100 read-request, 0b101 read-response,
//           0b110 digest-request, 0b111 digest-response.
//
// The digest request carries the range in place of a payload - offset (u32, the header's own
// slot) followed by a u16 length - and its response echoes the offset and carries a u32 CRC32
// of exactly [offset, offset+length) of the app region: start | control | offset | length | end
// = 9 bytes, and start | control | offset | crc | end = 11 bytes. Bytes past the end of the app
// region read as 0xFF, so a tool asks with a padded length and a correct image still matches.
// CRC32 is the reflected 0xEDB88320 polynomial, init 0xFFFFFFFF, final XOR 0xFFFFFFFF (the
// CRC-32/ISO-HDLC that zlib.crc32 computes).
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
    constexpr uint8_t CMD_DIGEST_REQ    = 0x6; // 0b110 - CRC32 of a range of the app region
    constexpr uint8_t CMD_DIGEST_RESP   = 0x7; // 0b111 - the CRC, offset echoed

    // Decode sentinel: 0 is a valid command (identify-request), so "invalid" is 0xFF.
    constexpr uint8_t INVALID = 0xFF;

    constexpr uint8_t PAYLOAD_SIZE   = 32;
    constexpr uint8_t HEADER_SIZE    = 6;  // start + control + offset
    constexpr uint8_t READ_REQ_SIZE  = HEADER_SIZE + 1;                    // header + end
    constexpr uint8_t DATA_SIZE      = HEADER_SIZE + PAYLOAD_SIZE + 1;     // header + payload + end
    // Digest request: offset (u32) + length (u16) after the header is 2 + 1, response: the CRC
    // (u32) + end. Both stay well inside MAX_FRAME_SIZE.
    constexpr uint8_t DIGEST_REQ_SIZE  = HEADER_SIZE + 2 + 1;
    constexpr uint8_t DIGEST_RESP_SIZE = HEADER_SIZE + 4 + 1;
    constexpr uint8_t NAME_SIZE      = 32; // identify-response name (null-padded)
    constexpr uint8_t MAX_FRAME_SIZE = DATA_SIZE;

    // Even parity over the command (3 bits), the offset and the payload (bytes 2..len-2).
    //
    // Parity of a XOR is the XOR of the parities, so the whole body collapses to one byte with
    // a single XOR per byte and one fold at the end - instead of the shift-and-xor ladder run
    // per byte. The command's three bits join the same sum (cmd is masked to 3 bits, so its
    // higher bits cannot contribute).
    constexpr uint8_t Parity(const uint8_t *Frame, uint16_t Len)
    {
        uint8_t Acc = (uint8_t)(Frame[1] & 0x07u);
        for (uint16_t i = 2; i + 1 < Len; i++)
            Acc ^= Frame[i];
        Acc ^= (uint8_t)(Acc >> 4);
        Acc ^= (uint8_t)(Acc >> 2);
        Acc ^= (uint8_t)(Acc >> 1);
        return (uint8_t)(Acc & 1u);
    }

    // Fills byte 1: padding 0, even parity, command. `Frame` must already hold the full body.
    inline void WriteControl(uint8_t *Frame, uint8_t Cmd, uint16_t Len)
    {
        Frame[1] = (uint8_t)(Cmd & 0x07u);
        Frame[1] |= (uint8_t)(Parity(Frame, Len) << 3);
    }

    // The two frame layouts every encoder below is built from: [start][control][offset][payload][end]
    // and its header-only form (no payload). Kept here so the four encoders cannot drift apart in
    // the bytes they share - each is now only its command plus one of these bodies.
    inline void EncodeDataBody(uint32_t Offset, const uint8_t *Payload, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        memcpy(Out + 6, Payload, PAYLOAD_SIZE);
        Out[DATA_SIZE - 1] = END;
    }

    inline void EncodeHeaderBody(uint32_t Offset, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        Out[READ_REQ_SIZE - 1] = END;
    }

    inline void EncodeWrite(uint32_t Offset, const uint8_t *Payload, uint8_t *Out)
    {
        EncodeDataBody(Offset, Payload, Out);
        WriteControl(Out, CMD_WRITE, DATA_SIZE);
    }

    // Acknowledges a write: header-only, carrying the offset of the last successfully written chunk.
    inline void EncodeWriteResponse(uint32_t Offset, uint8_t *Out)
    {
        EncodeHeaderBody(Offset, Out);
        WriteControl(Out, CMD_WRITE_RESP, READ_REQ_SIZE);
    }

    inline void EncodeReadRequest(uint32_t Offset, uint8_t *Out)
    {
        EncodeHeaderBody(Offset, Out);
        WriteControl(Out, CMD_READ_REQ, READ_REQ_SIZE);
    }

    inline void EncodeReadResponse(uint32_t Offset, const uint8_t *Payload, uint8_t *Out)
    {
        EncodeDataBody(Offset, Payload, Out);
        WriteControl(Out, CMD_READ_RESP, DATA_SIZE);
    }

    // A read-response frame whose payload slot the caller has already filled. The node copies
    // the 32 bytes straight from flash into `Out + 6`, so it needs the frame shell and the
    // control byte but not the staging copy `EncodeReadResponse` makes.
    inline void EncodeReadFrame(uint32_t Offset, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        Out[DATA_SIZE - 1] = END;
        WriteControl(Out, CMD_READ_RESP, DATA_SIZE);
    }

    // Identify request: header-only, offset slot zero.
    inline void EncodeIdentifyRequest(uint8_t *Out)
    {
        EncodeHeaderBody(0, Out);
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

    // -----------------------------------------------------------------------
    // Writing one chunk (card t_5195d4bc: the erase decision and the ack rule)
    // -----------------------------------------------------------------------
    //
    // An upload sends 32-byte chunks in order (Docs/Services/Bootloader.md), so the SECOND chunk of
    // every 64-byte flash page lands in a page the first chunk just programmed.
    //
    // The erase decision is POSITIONAL, never content-based: only the chunk whose offset starts a
    // page erases that page, and it erases it whatever the page already holds - a page carrying an
    // older image is exactly what a re-flash has to replace, and a blank page only costs the erase.
    // A chunk inside a page never erases, because the erase unit is the whole page and clearing it
    // would take the chunk that just landed in the other half with it.
    //
    // The ack rule IS the verification: the write response names `Offset` only when the 32 bytes
    // read back as sent. Flash programming can only clear bits, so a chunk arriving in a slot that
    // was never erased reads back different and is refused (the response keeps the previous
    // offset, which is how the tool tells a refusal from an ack - Docs: "Success only if the offset
    // matches the request"), while a re-sent identical chunk verifies and is acked, so the
    // protocol stays idempotent for the app's retry round.
    //
    // `Flash` is the device's flash adapter (Unlock/Lock, ErasePage, ProgramWord, ReadWord). It is
    // a template parameter so the host test drives THIS function against a NOR model (program only
    // clears bits) instead of a copy of the rule.
    constexpr uint32_t PAGE_SIZE = 64; // the CH32V003 CR_PAGE_ER erase page

    template <class Flash>
    inline bool WriteChunk(uint32_t Offset, const uint8_t *Payload, uint32_t AppSize,
                           uint32_t &LastWritten)
    {
        if ((Offset & (PAYLOAD_SIZE - 1u)) != 0) return false; // 32-byte aligned
        // Bounds are checked on the offset, not the computed address: `AppSize - Offset` under an
        // `Offset <= AppSize` guard cannot wrap.
        if (Offset > AppSize || AppSize - Offset < PAYLOAD_SIZE) return false;
        if ((Offset & (PAGE_SIZE - 1u)) == 0 && !Flash::ErasePage(Offset)) return false;

        // Program and read back in ONE loop: the words are compared while the chunk is still in
        // hand, so the ack above costs no second pass over the payload.
        bool Landed = true;
        Flash::Unlock();
        for (uint8_t i = 0; i < PAYLOAD_SIZE; i += 4)
        {
            uint32_t Word;
            memcpy(&Word, Payload + i, sizeof(Word));
            Flash::ProgramWord(Offset + i, Word);
            if (Flash::ReadWord(Offset + i) != Word) Landed = false;
        }
        Flash::Lock();
        if (!Landed) return false;
        LastWritten = Offset;
        return true;
    }

    // -----------------------------------------------------------------------
    // CRC32 (the digest command)
    // -----------------------------------------------------------------------
    //
    // One step of the reflected CRC-32/ISO-HDLC (poly 0xEDB88320), fed byte by byte. Exposed as
    // a step so the node can run one loop over a range that runs past the end of the app region
    // (padding with 0xFF) without a second copy of the loop - and without a 1 KB table, which
    // the 2 KB bootloader does not have.
    constexpr uint32_t Crc32Step(uint32_t Crc, uint8_t Byte)
    {
        Crc ^= Byte;
        for (uint8_t Bit = 0; Bit < 8; Bit++)
            Crc = (Crc >> 1) ^ ((Crc & 1u) ? 0xEDB88320u : 0u);
        return Crc;
    }

    // CRC32 of a whole buffer: init 0xFFFFFFFF, final XOR 0xFFFFFFFF (zlib.crc32's value).
    inline uint32_t Crc32(const uint8_t *Data, uint32_t Len)
    {
        uint32_t Crc = 0xFFFFFFFFu;
        while (Len--) Crc = Crc32Step(Crc, *Data++);
        return Crc ^ 0xFFFFFFFFu;
    }

    // Digest request: offset (u32) in the header's slot, then the length (u16).
    inline void EncodeDigestRequest(uint32_t Offset, uint16_t Length, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        memcpy(Out + 6, &Length, 2);
        Out[DIGEST_REQ_SIZE - 1] = END;
        WriteControl(Out, CMD_DIGEST_REQ, DIGEST_REQ_SIZE);
    }

    // Digest response: the offset echoed in the header's slot, then the CRC32 (u32).
    inline void EncodeDigestResponse(uint32_t Offset, uint32_t Crc, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Offset, 4);
        memcpy(Out + 6, &Crc, 4);
        Out[DIGEST_RESP_SIZE - 1] = END;
        WriteControl(Out, CMD_DIGEST_RESP, DIGEST_RESP_SIZE);
    }

    inline uint16_t DigestLength(const uint8_t *Frame)
    {
        return (uint16_t)(Frame[6] | ((uint16_t)Frame[7] << 8));
    }

    inline uint32_t DigestCrc(const uint8_t *Frame)
    {
        return (uint32_t)Frame[6] | ((uint32_t)Frame[7] << 8) | ((uint32_t)Frame[8] << 16) |
               ((uint32_t)Frame[9] << 24);
    }

    // The frame length for a command, or 0 when the command is unknown. One byte per command
    // in flash measures smaller than the compare chain on this target.
    constexpr uint8_t FRAME_SIZE_OF[8] = {
        READ_REQ_SIZE,     // 0b000 identify-request
        DATA_SIZE,         // 0b001 identify-response
        DATA_SIZE,         // 0b010 write
        READ_REQ_SIZE,     // 0b011 write-response
        READ_REQ_SIZE,     // 0b100 read-request
        DATA_SIZE,         // 0b101 read-response
        DIGEST_REQ_SIZE,   // 0b110 digest-request
        DIGEST_RESP_SIZE   // 0b111 digest-response
    };

    inline uint16_t FrameSize(uint8_t Cmd)
    {
        return Cmd < 8 ? FRAME_SIZE_OF[Cmd] : 0;
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

    // What one byte read back during a raw send actually is (card t_d9feed1d).
    //
    // A raw request is verified by its echo on the half-duplex line, but a node in bootloader
    // answers ONE SILENCE WINDOW after our last byte - BL_SILENCE_BYTES = 32 byte times, ~704 us
    // at 460800 - and that reply begins inside the echo window of the last chunk. A chunk-wide
    // read compared against what was sent therefore compares the START of the node's reply
    // against our own bytes: it mismatches, the reply's head is thrown away with the chunk, and
    // the request is reported as a collision - which is why read/write never completed while
    // identify (a longer answer) did.
    //
    // Classifying each byte instead keeps the reply: a byte that is not our echo but IS the
    // start marker means the node has already accepted the whole frame and is answering, which
    // is the expected case here, not an error. Only a byte that is neither is a real collision.
    enum class EchoByte : uint8_t
    {
        Echo,     // our own byte came back: keep verifying
        Reply,    // START: the node is answering - hand it, and everything after it, to the sink
        Collision // neither: another station is driving the line
    };

    constexpr EchoByte ClassifyEchoByte(uint8_t Sent, uint8_t Got)
    {
        if (Got == Sent)
            return EchoByte::Echo;
        return Got == START ? EchoByte::Reply : EchoByte::Collision;
    }
}
