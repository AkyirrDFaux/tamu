#pragma once

// Bootloader packet codec (Docs/Services/Bootloader.md), protocol v2.
//
// The bootloader replaces the main binary; it deliberately ignores the standard packet
// format and speaks this raw, byte-packed frame on USB (cores) or RSBus (nodes):
//
//   [0]      0xCA start marker
//   [1]      control: padding(3)=0 | parity(1, even) | command(4)
//   [2..5]   offset, u32 little-endian, relative to the main binary space start
//   [6..37]  32-byte payload  (write, read-response)
//   [38]     0xBC end marker  (write, read-response)
//
// Header-only frames (offset slot only): start | control | offset | end = 7 bytes.
// Commands: bit 0 is the direction (0 = request, 1 = response) and bits 1-3 the operation:
//           0b000x identify, 0b001x read, 0b010x erase, 0b011x write, 0b100x digest/CRC.
//           0b1010..0b1111 are undefined and rejected.
//
// v2 is the 4-bit command field: three bits were all used (eight commands) and the erase needed a
// ninth. The values are RENUMBERED - bits 1-3 are the operation, bit 0 the direction - so an opcode
// reads as what it does instead of as an arbitrary number, and the parity bit moved from bit 3 to
// bit 4 to make room, so a v2 frame is never a v1 frame. The identify response is 39 bytes in BOTH
// (v2 carries the page size in the last four bytes of v1's 32-byte name field, behind a name that
// is at most 28 bytes and NUL-padded), so the version is not visible in a frame length - the
// control byte is what tells the two apart.
//
// The digest request is the shortest frame there is - start | control | end, 3 bytes - because the
// CRC always covers the WHOLE app space: there is no offset and no length to carry. Its response
// echoes nothing either, it is start | control | crc32 | end = 7 bytes. The CRC is CRC32 of
// [0, app size) (the app size the identify response reports).
// Bytes past the end of the image read as their flash value (0xFF once the tail is blanked, which
// the upload procedure does), so the value is a function of the image alone.
// CRC32 is the reflected 0xEDB88320 polynomial, init 0xFFFFFFFF, final XOR 0xFFFFFFFF (the
// CRC-32/ISO-HDLC that zlib.crc32 computes).
//
// The identify response carries the app-space size in the offset slot (u32 LE), then the flash
// PAGE SIZE (u32, the erase unit of the erase command), then the device name as a 28-byte
// null-padded field: start | control | size | page | name | end = 39 bytes. The page size is a full
// 4-byte field because the erase units differ by device (a 64 B page on the CH32V003, a 4 KB
// sector on the CH32V203 and the ESP32-C3). Constants first, name last
// (Docs/Services/Bootloader.md, commit 7a45e00).
//
// The erase request names one page (any offset inside it) in the offset slot and is header-only;
// its response carries the page it blanked, with bit 0 set when that page is not blank. Keeping
// the erase in its own command is what lets a write be pure programming, and what lets a tool
// blank only the pages it is about to rewrite.
//
// Parity is even over the "rest of the packet" after the control byte's padding/parity: the
// command (4 bits), the offset (32 bits) and the payload (32 bytes, when present). The start
// marker, the 3 padding bits and the end marker are excluded.

#include <cstdint>
#include <cstring>

namespace Bootloader
{
    constexpr uint8_t START = 0xCA;
    constexpr uint8_t END   = 0xBC;

    constexpr uint8_t CMD_IDENTIFY_REQ  = 0x0; // 0b0000 - identify, request
    constexpr uint8_t CMD_IDENTIFY_RESP = 0x1; // 0b0001 - identify, response
    constexpr uint8_t CMD_READ_REQ      = 0x2; // 0b0010 - read, request
    constexpr uint8_t CMD_READ_RESP     = 0x3; // 0b0011 - read, response
    constexpr uint8_t CMD_ERASE_REQ     = 0x4; // 0b0100 - erase one page (the offset names it)
    constexpr uint8_t CMD_ERASE_RESP    = 0x5; // 0b0101 - the page it blanked, bit 0 = failure
    constexpr uint8_t CMD_WRITE         = 0x6; // 0b0110 - write one 32-byte chunk
    constexpr uint8_t CMD_WRITE_RESP    = 0x7; // 0b0111 - the last written offset
    constexpr uint8_t CMD_DIGEST_REQ    = 0x8; // 0b1000 - CRC32 of a range of the app region
    constexpr uint8_t CMD_DIGEST_RESP   = 0x9; // 0b1001 - the CRC, offset echoed

    // Decode sentinel: 0 is a valid command (identify-request), so "invalid" is 0xFF.
    constexpr uint8_t INVALID = 0xFF;

    constexpr uint8_t PAYLOAD_SIZE   = 32;
    constexpr uint8_t HEADER_SIZE    = 6;  // start + control + offset
    constexpr uint8_t READ_REQ_SIZE  = HEADER_SIZE + 1;                    // header + end
    constexpr uint8_t DATA_SIZE      = HEADER_SIZE + PAYLOAD_SIZE + 1;     // header + payload + end
    // Digest (CRC): the request carries nothing (start | control | end), the response the CRC (u32).
    constexpr uint8_t DIGEST_REQ_SIZE  = 3;                          // start | control | end
    constexpr uint8_t DIGEST_RESP_SIZE = 2 + 4 + 1;                  // start | control | crc | end
    // Erase: header-only both ways - the request names the page in the offset slot (any offset
    // inside it), the ack carries the page it blanked. One page per request, see ErasePage.
    constexpr uint8_t ERASE_REQ_SIZE   = HEADER_SIZE + 1;
    constexpr uint8_t ERASE_RESP_SIZE  = HEADER_SIZE + 1;
    constexpr uint8_t NAME_SIZE      = 28; // identify-response name (null-padded)
    // Identify response: app size (u32) in the offset slot, the flash page size (u32), then the
    // name. The page size is what the erase command's page is measured in, so the tool asks the
    // device instead of assuming 64: the erase units differ (a 64 B page on the CH32V003, a 4 KB
    // sector on the CH32V203 and the ESP32-C3), so a byte would not hold them. The constants come
    // first and the name last (Docs/Services/Bootloader.md, 7a45e00).
    constexpr uint8_t IDENTIFY_PAGE_AT   = HEADER_SIZE;                  // 6
    constexpr uint8_t IDENTIFY_NAME_AT   = HEADER_SIZE + 4;              // 10
    constexpr uint8_t IDENTIFY_RESP_SIZE = IDENTIFY_NAME_AT + NAME_SIZE + 1; // 39, the same as v1
    constexpr uint8_t MAX_FRAME_SIZE     = IDENTIFY_RESP_SIZE;

    // Even parity over the command (4 bits), the offset and the payload (bytes 2..len-2).
    //
    // Parity of a XOR is the XOR of the parities, so the whole body collapses to one byte with
    // a single XOR per byte and one fold at the end - instead of the shift-and-xor ladder run
    // per byte. The command's four bits join the same sum (cmd is masked to 4 bits, so its
    // higher bits cannot contribute).
    constexpr uint8_t Parity(const uint8_t *Frame, uint16_t Len)
    {
        uint8_t Acc = (uint8_t)(Frame[1] & 0x0Fu);
        for (uint16_t i = 2; i + 1 < Len; i++)
            Acc ^= Frame[i];
        Acc ^= (uint8_t)(Acc >> 4);
        Acc ^= (uint8_t)(Acc >> 2);
        Acc ^= (uint8_t)(Acc >> 1);
        return (uint8_t)(Acc & 1u);
    }

    // Fills byte 1: command(4) | parity(1) | padding(3)=0. `Frame` must already hold the full body.
    inline void WriteControl(uint8_t *Frame, uint8_t Cmd, uint16_t Len)
    {
        Frame[1] = (uint8_t)(Cmd & 0x0Fu);
        Frame[1] |= (uint8_t)(Parity(Frame, Len) << 4);
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

    // A header-only frame: [start][control][offset][end]. The offset slot is the whole payload, so
    // ONE builder serves every request and ack of that shape - identify and read requests, write
    // and erase acks - instead of one copy of the same six bytes per command.
    inline void EncodeHeaderFrame(uint8_t Cmd, uint32_t Offset, uint8_t *Out)
    {
        EncodeHeaderBody(Offset, Out);
        WriteControl(Out, Cmd, READ_REQ_SIZE);
    }

    // Acknowledges a write: header-only, carrying the offset of the last successfully written chunk.
    inline void EncodeWriteResponse(uint32_t Offset, uint8_t *Out)
    {
        EncodeHeaderFrame(CMD_WRITE_RESP, Offset, Out);
    }

    // Acknowledges an erase: header-only, carrying the page (bit 0 set when it is not blank).
    inline void EncodeEraseResponse(uint32_t Offset, uint8_t *Out)
    {
        EncodeHeaderFrame(CMD_ERASE_RESP, Offset, Out);
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

    // Identify response: app-space size (u32) in the offset slot, the flash page size (u32), then
    // the name (28 B, NUL-padded) - constants first, name last.
    inline void EncodeIdentifyResponse(uint32_t AppSize, uint32_t PageSize, const char *Name,
                                       uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &AppSize, 4);
        memcpy(Out + IDENTIFY_PAGE_AT, &PageSize, 4);
        memset(Out + IDENTIFY_NAME_AT, 0, NAME_SIZE);
        uint8_t i = 0;
        while (i < NAME_SIZE && Name[i] != '\0')
        {
            Out[IDENTIFY_NAME_AT + i] = (uint8_t)Name[i];
            i++;
        }
        Out[IDENTIFY_RESP_SIZE - 1] = END;
        WriteControl(Out, CMD_IDENTIFY_RESP, IDENTIFY_RESP_SIZE);
    }

    // The identify response's geometry field, and the name that follows it.
    inline uint32_t IdentifyPageSize(const uint8_t *Frame)
    {
        return (uint32_t)Frame[IDENTIFY_PAGE_AT] |
               ((uint32_t)Frame[IDENTIFY_PAGE_AT + 1] << 8) |
               ((uint32_t)Frame[IDENTIFY_PAGE_AT + 2] << 16) |
               ((uint32_t)Frame[IDENTIFY_PAGE_AT + 3] << 24);
    }
    inline const uint8_t *IdentifyName(const uint8_t *Frame) { return Frame + IDENTIFY_NAME_AT; }

    inline uint32_t Offset(const uint8_t *Frame)
    {
        uint32_t Offset;
        memcpy(&Offset, Frame + 2, 4);
        return Offset;
    }

    inline const uint8_t *Payload(const uint8_t *Frame) { return Frame + 6; }

    // -----------------------------------------------------------------------
    // Writing one chunk (cards t_5195d4bc, t_cb8bd722 and the v2 erase split)
    // -----------------------------------------------------------------------
    //
    // A write only programs. It used to own the erase: the chunk whose offset started a 64-byte
    // page erased it, and a chunk inside a page was refused unless the page's first half was
    // known to be there. That rule existed because the erase was a side effect of a write, and
    // it is what made a lost page-start write leave a page blank for ever (E85). With ERASE as
    // its own command the tool blanks the pages it is about to write first, so:
    //   * a lost write costs one re-sent write, never a blank page (E85's shape is gone);
    //   * a delta upload blanks exactly the pages that differ;
    //   * a write no longer needs to know what the page holds - except for the one check below.
    //
    // The one check: a PAGE-START chunk (offset % PAGE_SIZE == 0) reads the page's first word
    // before it programs anything, and refuses with the PAGE named when that word is not
    // 0xFFFFFFFF. It catches a tool writing into a page it never erased (a lost erase ack, a
    // tool that assumes the old erase-in-write behaviour), which would otherwise program into
    // dirty bits and read back different. A chunk INSIDE a page is never checked: by then its
    // page's first word is the one this very upload just programmed. The check is unconditional -
    // it costs 28 B of the 2 KB region and the owner allowed it because it fit (2026-11-13).
    //
    // The ack rule IS the verification: the write response names `Offset` only when the 32 bytes
    // read back as sent, and names the PAGE when the page could not be written. Flash programming
    // can only clear bits, so a chunk arriving in a slot that was not blank reads back different
    // and is refused (the response keeps the previous offset, which is how the tool tells a
    // refusal from an ack - Docs: "Success only if the offset matches the request"). A re-sent
    // chunk is NOT idempotent across a page start: once the page's first word holds data, the
    // repeated PAGE-START chunk is refused by the check above, so a retry of a lost ack has to
    // come either before the page's first chunk lands or after an erase of that page.
    //
    // `Flash` is the device's flash adapter (Unlock/Lock, ErasePage, ProgramWord, ReadWord). It is
    // a template parameter so the host test drives THIS function against a NOR model (program only
    // clears bits) instead of a copy of the rule.
    constexpr uint32_t PAGE_SIZE = 64; // the CH32V003 CR_PAGE_ER erase page

    // What one write request did, which is what its response has to say:
    enum class ChunkResult : uint8_t
    {
        Refused,   // misaligned or out of range: nothing was touched, the ack keeps the old offset
        Written,   // the chunk is in flash: the ack names this chunk
        PageFailed // the page could not be written: the ack names the PAGE, so the tool re-sends it
    };

    template <class Flash>
    inline ChunkResult WriteChunk(uint32_t Offset, const uint8_t *Payload, uint32_t AppSize,
                                  uint32_t &LastWritten)
    {
        if ((Offset & (PAYLOAD_SIZE - 1u)) != 0) return ChunkResult::Refused; // 32-byte aligned
        // Bounds are checked on the offset, not the computed address: `AppSize - Offset` under an
        // `Offset <= AppSize` guard cannot wrap.
        if (Offset > AppSize || AppSize - Offset < PAYLOAD_SIZE) return ChunkResult::Refused;

        // The erase is the ERASE command's job, not this one, so a write only ever programs. A
        // PAGE-START chunk also proves the erase happened: its page's first word must read
        // 0xFFFFFFFF, or the tool is writing into a page it never erased (a lost erase ack, an
        // old tool), and the page is named instead. A chunk INSIDE a page is never checked - by
        // then the page's first word is the one this upload just programmed.
        if ((Offset & (PAGE_SIZE - 1u)) == 0 && Flash::ReadWord(Offset) != 0xFFFFFFFFu)
            return ChunkResult::PageFailed;

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
        if (!Landed) return ChunkResult::PageFailed;
        LastWritten = Offset;
        return ChunkResult::Written;
    }

    // The offset a write response carries for one outcome - the whole answer the tool has to read:
    // the chunk when it landed, the PAGE (with bit 0 set as the failure flag) when the page could not
    // be written, and the previous offset when nothing was touched. Bit 0 is the flag because every
    // real offset is 32-byte aligned; without it a failed PAGE-START chunk would answer with its own
    // offset and be read as a success.
    inline uint32_t WriteAck(ChunkResult Result, uint32_t Offset, uint32_t LastWritten)
    {
        return Result == ChunkResult::PageFailed ? ((Offset & ~(PAGE_SIZE - 1u)) | 1u) : LastWritten;
    }

    // -----------------------------------------------------------------------
    // Erasing a page (the ERASE command)
    // -----------------------------------------------------------------------
    //
    // The erase is its own command, which is what lets a write be pure programming: the tool
    // blanks the pages it is about to rewrite, then sends the 32-byte chunks. Three things
    // follow. A lost write can no longer leave a page erased - the erase happened on purpose,
    // before the chunks, so a lost write is re-sent as a write. A delta upload can blank exactly
    // the pages that differ instead of the whole region. And the erase no longer has to be
    // decided from the OFFSET inside a write, so WriteChunk holds no page rule at all.
    //
    // ONE page per request, not a range: the loop over a range costs 104 B of this 2 KB image
    // (measured), while a tool that sends one request per page pays only bus time, of which the
    // side that has 2 KB of flash has none. The offset must be PAGE-ALIGNED: an erase is the one
    // destructive command, so a misaligned offset is REFUSED with the flag and never rounded into
    // the page below it - the refusal is what catches a tool computing a bad offset, and rounding
    // would blank a page the tool never asked about.
    //
    // The answer is the page, with bit 0 set when it is NOT blank (the same flag WriteAck uses:
    // a real page offset is 64-byte aligned, so bit 0 is never set in a plain ack). A page past
    // the region the caller allows is answered the same way and is not touched, so an ack always
    // means "this page is blank now" - which is what the writes that follow rely on. A misaligned
    // offset is answered with the offending OFFSET and the same flag, since it names no page.
    template <class Flash>
    inline uint32_t ErasePage(uint32_t Offset, uint32_t AppSize)
    {
        if ((Offset & (PAGE_SIZE - 1u)) != 0) return Offset | 1u;
        // Aligned past the line above, so `Offset` IS the page and no masking is needed.
        if (Offset >= AppSize || !Flash::ErasePage(Offset)) return Offset | 1u;
        return Offset;
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

    // Digest request: the shortest frame - start | control | end. The CRC covers the whole app
    // space, so there is nothing to carry.
    inline void EncodeDigestRequest(uint8_t *Out)
    {
        Out[0] = START;
        Out[DIGEST_REQ_SIZE - 1] = END;
        WriteControl(Out, CMD_DIGEST_REQ, DIGEST_REQ_SIZE);
    }

    // Erase request: header-only, naming the 64-byte page with its own base offset.
    inline void EncodeEraseRequest(uint32_t Offset, uint8_t *Out)
    {
        EncodeHeaderFrame(CMD_ERASE_REQ, Offset, Out);
    }

    // Digest response: the CRC32 of the whole app space (u32) between the control byte and the end.
    inline void EncodeDigestResponse(uint32_t Crc, uint8_t *Out)
    {
        Out[0] = START;
        memcpy(Out + 2, &Crc, 4);
        Out[DIGEST_RESP_SIZE - 1] = END;
        WriteControl(Out, CMD_DIGEST_RESP, DIGEST_RESP_SIZE);
    }

    // The u32 CRC of a digest response.
    inline uint32_t DigestCrc(const uint8_t *Frame)
    {
        return (uint32_t)Frame[2] | ((uint32_t)Frame[3] << 8) | ((uint32_t)Frame[4] << 16) |
               ((uint32_t)Frame[5] << 24);
    }

    // The frame length for a command, or 0 when the command is unknown. One byte per command
    // in flash measures smaller than the compare chain on this target.
    constexpr uint8_t FRAME_SIZE_OF[16] = {
        READ_REQ_SIZE,      // 0b0000 identify-request
        IDENTIFY_RESP_SIZE, // 0b0001 identify-response
        READ_REQ_SIZE,      // 0b0010 read-request
        DATA_SIZE,          // 0b0011 read-response
        ERASE_REQ_SIZE,     // 0b0100 erase-request
        ERASE_RESP_SIZE,    // 0b0101 erase-response
        DATA_SIZE,          // 0b0110 write
        READ_REQ_SIZE,      // 0b0111 write-response
        DIGEST_REQ_SIZE,    // 0b1000 digest-request
        DIGEST_RESP_SIZE,   // 0b1001 digest-response
        0,                  // 0b1010
        0,                  // 0b1011
        0,                  // 0b1100
        0,                  // 0b1101
        0,                  // 0b1110
        0                   // 0b1111
    };

    inline uint16_t FrameSize(uint8_t Cmd)
    {
        return Cmd < 16 ? FRAME_SIZE_OF[Cmd] : 0;
    }

    // Validates a complete frame; returns the command, or INVALID when the frame is bad.
    inline uint8_t Decode(const uint8_t *Frame, uint16_t Len)
    {
        if (Len < DIGEST_REQ_SIZE || Frame[0] != START) // 3 bytes: the shortest frame there is
            return INVALID;
        if ((Frame[1] & 0xE0u) != 0) // padding (bits 5-7) must be zero
            return INVALID;
        uint8_t Cmd = (uint8_t)(Frame[1] & 0x0Fu);
        uint16_t Expect = FrameSize(Cmd);
        if (Expect == 0 || Len != Expect || Frame[Len - 1] != END)
            return INVALID;
        uint8_t Got = (uint8_t)((Frame[1] >> 4) & 1u);
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
