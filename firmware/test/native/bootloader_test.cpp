// Host-side test for the bootloader packet codec (Core/Functions/Bootloader.h).
//
// The codec is the contract between the app and the bootloader (Docs/Services/Bootloader.md),
// so this pins the byte layout, the even parity and the frame sizes. The parity reference below
// is deliberately independent (a plain bit walk) so a broken fold in the codec cannot agree
// with itself. The KAT hex strings at the end are mirrored verbatim in the app test.
//
// Protocol v2: the control byte is padding(3)=0 | parity(1) | command(4), so ten commands exist
// (the digest pair plus ERASE) and Decode's "invalid" sentinel is 0xFF (0b0000 is a valid
// command: identify-request).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/Bootloader.h"

#include "check.h"

// Independent even parity over the "rest of the packet": the command (byte 1, bits 0-3), the
// offset and the payload (bytes 2..len-2). The markers and the padding bits are excluded.
static uint8_t RefParity(const uint8_t *Frame, uint16_t len)
{
    uint8_t Parity = 0;
    for (int bit = 0; bit < 4; bit++)
        Parity ^= (uint8_t)((Frame[1] >> bit) & 1u);
    for (uint16_t i = 2; i + 1 < len; i++)
        for (int bit = 0; bit < 8; bit++)
            Parity ^= (uint8_t)((Frame[i] >> bit) & 1u);
    return (uint8_t)(Parity & 1u);
}

static void PrintHex(const char *label, const uint8_t *Frame, uint16_t len)
{
    std::printf("KAT %s = ", label);
    for (uint16_t i = 0; i < len; i++) std::printf("%02X", Frame[i]);
    std::printf("\n");
}

int main()
{
    using namespace Bootloader;

    // --- the control byte's shape -------------------------------------------
    // bit 0 = direction (0 request, 1 response), bits 1-3 = operation.
    CHECK_EQ(CMD_IDENTIFY_REQ, 0x0u);          // 0b0000
    CHECK_EQ(CMD_IDENTIFY_RESP, 0x1u);         // 0b0001
    CHECK_EQ(CMD_READ_REQ, 0x2u);              // 0b0010
    CHECK_EQ(CMD_READ_RESP, 0x3u);             // 0b0011
    CHECK_EQ(CMD_ERASE_REQ, 0x4u);             // 0b0100
    CHECK_EQ(CMD_ERASE_RESP, 0x5u);            // 0b0101
    CHECK_EQ(CMD_WRITE, 0x6u);                 // 0b0110
    CHECK_EQ(CMD_WRITE_RESP, 0x7u);            // 0b0111
    CHECK_EQ(CMD_DIGEST_REQ, 0x8u);            // 0b1000
    CHECK_EQ(CMD_DIGEST_RESP, 0x9u);           // 0b1001
    CHECK_EQ(CMD_IDENTIFY_REQ & 1u, 0u);       // identify: a request
    CHECK_EQ(CMD_IDENTIFY_RESP & 1u, 1u);      // ... and its response
    CHECK_EQ(CMD_READ_REQ & 1u, 0u);
    CHECK_EQ(CMD_READ_RESP & 1u, 1u);
    CHECK_EQ(CMD_ERASE_REQ & 1u, 0u);
    CHECK_EQ(CMD_ERASE_RESP & 1u, 1u);
    CHECK_EQ(CMD_WRITE & 1u, 0u);
    CHECK_EQ(CMD_WRITE_RESP & 1u, 1u);
    CHECK_EQ(CMD_DIGEST_REQ & 1u, 0u);
    CHECK_EQ(CMD_DIGEST_RESP & 1u, 1u);
    CHECK_EQ(CMD_WRITE & 0x0Fu, 0x6u);
    CHECK_EQ(CMD_ERASE_REQ, 0x4u);
    CHECK_EQ(CMD_ERASE_RESP, 0x5u);
    CHECK_EQ(IDENTIFY_RESP_SIZE, 39);          // the same length as v1 - see the identify block
    CHECK_EQ(IDENTIFY_RESP_SIZE, DATA_SIZE);
    CHECK_EQ(ERASE_REQ_SIZE, READ_REQ_SIZE);   // header-only both ways
    CHECK_EQ(ERASE_RESP_SIZE, READ_REQ_SIZE);

    // --- write frame layout -------------------------------------------------
    {
        uint8_t PayloadBuf[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) PayloadBuf[i] = (uint8_t)i;
        uint8_t Frame[DATA_SIZE];
        EncodeWrite(0x00000000u, PayloadBuf, Frame);

        CHECK_EQ(sizeof(Frame), 39);
        CHECK_EQ(Frame[0], START);
        CHECK_EQ(Frame[38], END);
        CHECK_EQ(Frame[1] & 0x0Fu, CMD_WRITE);
        CHECK_EQ(Frame[1] & 0xE0u, 0);            // padding zero (bits 5-7)
        CHECK_EQ((Frame[1] >> 4) & 1u, RefParity(Frame, DATA_SIZE));
        CHECK_EQ(Offset(Frame), 0u);
        CHECK(std::memcmp(Payload(Frame), PayloadBuf, PAYLOAD_SIZE) == 0);
        CHECK_EQ(Decode(Frame, DATA_SIZE), CMD_WRITE);
    }

    // --- read-request layout ------------------------------------------------
    {
        uint8_t Frame[READ_REQ_SIZE];
        EncodeReadRequest(0x12345678u, Frame);
        CHECK_EQ(sizeof(Frame), 7);
        CHECK_EQ(Frame[0], START);
        CHECK_EQ(Frame[6], END);
        CHECK_EQ(Frame[1] & 0x0Fu, CMD_READ_REQ);
        CHECK_EQ((Frame[1] >> 4) & 1u, RefParity(Frame, READ_REQ_SIZE));
        CHECK_EQ(Offset(Frame), 0x12345678u);
        CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_READ_REQ);
    }

    // --- read-response layout ----------------------------------------------
    {
        uint8_t PayloadBuf[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) PayloadBuf[i] = (uint8_t)(0xFF - i);
        uint8_t Frame[DATA_SIZE];
        EncodeReadResponse(0x00000040u, PayloadBuf, Frame);
        CHECK_EQ(Frame[0], START);
        CHECK_EQ(Frame[38], END);
        CHECK_EQ(Frame[1] & 0x0Fu, CMD_READ_RESP);
        CHECK_EQ((Frame[1] >> 4) & 1u, RefParity(Frame, DATA_SIZE));
        CHECK_EQ(Offset(Frame), 0x00000040u);
        CHECK(std::memcmp(Payload(Frame), PayloadBuf, PAYLOAD_SIZE) == 0);
        CHECK_EQ(Decode(Frame, DATA_SIZE), CMD_READ_RESP);
    }

    // --- write-response layout (header-only, carries the last written offset) ---
    {
        uint8_t Frame[READ_REQ_SIZE];
        EncodeWriteResponse(0x00001040u, Frame);
        CHECK_EQ(sizeof(Frame), 7);
        CHECK_EQ(Frame[0], START);
        CHECK_EQ(Frame[6], END);
        CHECK_EQ(Frame[1] & 0x0Fu, CMD_WRITE_RESP);
        CHECK_EQ((Frame[1] >> 4) & 1u, RefParity(Frame, READ_REQ_SIZE));
        CHECK_EQ(Offset(Frame), 0x00001040u);
        CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_WRITE_RESP);
    }

    // --- erase: request header-only, response names the page (bit 0 = not blank) --
    {
        uint8_t Req[ERASE_REQ_SIZE];
        EncodeEraseRequest(0x00003440u, Req);          // the page's own base offset
        CHECK_EQ(sizeof(Req), 7);
        CHECK_EQ(Req[0], START);
        CHECK_EQ(Req[6], END);
        CHECK_EQ(Req[1] & 0x0Fu, CMD_ERASE_REQ);
        CHECK_EQ(Req[1] & 0xE0u, 0);
        CHECK_EQ((Req[1] >> 4) & 1u, RefParity(Req, ERASE_REQ_SIZE));
        CHECK_EQ(Offset(Req), 0x00003440u);
        CHECK_EQ(Decode(Req, ERASE_REQ_SIZE), CMD_ERASE_REQ);

        uint8_t Resp[ERASE_RESP_SIZE];
        EncodeEraseResponse(0x00003440u, Resp);
        CHECK_EQ(Resp[0], START);
        CHECK_EQ(Resp[6], END);
        CHECK_EQ(Resp[1] & 0x0Fu, CMD_ERASE_RESP);
        CHECK_EQ((Resp[1] >> 4) & 1u, RefParity(Resp, ERASE_RESP_SIZE));
        CHECK_EQ(Offset(Resp), 0x00003440u);
        CHECK_EQ(Decode(Resp, ERASE_RESP_SIZE), CMD_ERASE_RESP);

        // A failed erase answers the page with bit 0 set - a value no page offset can take.
        EncodeEraseResponse(0x00003440u | 1u, Resp);
        CHECK_EQ(Offset(Resp), 0x00003441u);
        CHECK_EQ(Decode(Resp, ERASE_RESP_SIZE), CMD_ERASE_RESP);

        // Every single-bit flip of either frame must be rejected.
        for (uint16_t i = 0; i < ERASE_REQ_SIZE; i++)
            for (int bit = 0; bit < 8; bit++)
            {
                uint8_t Bad[ERASE_REQ_SIZE];
                std::memcpy(Bad, Req, ERASE_REQ_SIZE);
                Bad[i] ^= (uint8_t)(1u << bit);
                CHECK_EQ(Decode(Bad, ERASE_REQ_SIZE), INVALID);
            }

        // An erase request is 7 bytes like a read request: the command is what tells them apart,
        // not the length, so a read request must not decode as an erase.
        uint8_t Read[READ_REQ_SIZE];
        EncodeReadRequest(0x40u, Read);
        CHECK_EQ(Decode(Read, READ_REQ_SIZE), CMD_READ_REQ);

        PrintHex("erasereq", Req, ERASE_REQ_SIZE);
        PrintHex("eraseresp", Resp, ERASE_RESP_SIZE);
    }

    // --- identify: request header-only, response carries size + name + page ----
    {
        uint8_t req[READ_REQ_SIZE];
        EncodeIdentifyRequest(req);
        CHECK_EQ(req[0], START);
        CHECK_EQ(req[6], END);
        CHECK_EQ(req[1] & 0x0Fu, CMD_IDENTIFY_REQ);
        CHECK_EQ(Offset(req), 0u);
        // 0b0000 is a valid command: Decode must NOT report it as invalid.
        CHECK_EQ(Decode(req, READ_REQ_SIZE), CMD_IDENTIFY_REQ);

        uint8_t resp[IDENTIFY_RESP_SIZE];
        EncodeIdentifyResponse(0x00003600u, 64, "DAS v0.1", resp);
        CHECK_EQ(sizeof(resp), 39);                    // v1's length, unchanged
        CHECK_EQ(resp[0], START);
        CHECK_EQ(resp[38], END);
        CHECK_EQ(resp[1] & 0x0Fu, CMD_IDENTIFY_RESP);
        CHECK_EQ(resp[1] & 0xE0u, 0);
        CHECK_EQ((resp[1] >> 4) & 1u, RefParity(resp, IDENTIFY_RESP_SIZE));
        CHECK_EQ(Offset(resp), 0x00003600u);
        CHECK_EQ(IdentifyPageSize(resp), 64);          // the erase unit, u32 little-endian
        CHECK_EQ(resp[6], 64);                        // ... right after the app size
        CHECK_EQ(resp[7], 0);
        CHECK_EQ(IDENTIFY_PAGE_AT, 6);                 // constants first ...
        CHECK_EQ(IDENTIFY_NAME_AT, 10);                // ... name last
        CHECK(std::memcmp(IdentifyName(resp), "DAS v0.1", 9) == 0);
        CHECK_EQ(IdentifyName(resp)[9], 0);            // null-padded
        CHECK_EQ(IdentifyName(resp)[NAME_SIZE - 1], 0);
        CHECK_EQ(Decode(resp, IDENTIFY_RESP_SIZE), CMD_IDENTIFY_RESP);

        // The page size is a full u32, so a 4 KB sector is expressible; a name longer than the
        // field is truncated, never spilling into the end marker.
        uint8_t big[IDENTIFY_RESP_SIZE];
        EncodeIdentifyResponse(0x00100000u, 4096, "Tamu v2.0A", big);
        CHECK_EQ(IdentifyPageSize(big), 4096);
        CHECK_EQ(Decode(big, IDENTIFY_RESP_SIZE), CMD_IDENTIFY_RESP);
        uint8_t truncated[IDENTIFY_RESP_SIZE];
        EncodeIdentifyResponse(0, 64, "012345678901234567890123456789", truncated); // 30 chars
        CHECK(std::memcmp(IdentifyName(truncated), "0123456789012345678901234567", NAME_SIZE) == 0);
        CHECK_EQ(truncated[IDENTIFY_RESP_SIZE - 1], END); // the 29th and 30th chars are dropped
        CHECK_EQ(IdentifyPageSize(truncated), 64);        // the page field is untouched
        CHECK_EQ(Decode(truncated, IDENTIFY_RESP_SIZE), CMD_IDENTIFY_RESP);

        // A v1 control byte (parity at bit 3) must not decode. The length cannot tell the two
        // protocols apart - both identify responses are 39 bytes - so the control byte is what
        // does, and this is the request that pins it.
        uint8_t v1[READ_REQ_SIZE];
        EncodeReadRequest(0x00000101u, v1);            // three set bits: v1 parity would be 1
        v1[1] = (uint8_t)(CMD_READ_REQ | (1u << 3));
        CHECK_EQ(Decode(v1, READ_REQ_SIZE), INVALID);
    }

    // --- every single-bit flip must be rejected -----------------------------
    {
        uint8_t PayloadBuf[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) PayloadBuf[i] = (uint8_t)(i * 3 + 1);
        uint8_t Frame[DATA_SIZE];
        EncodeWrite(0x00000100u, PayloadBuf, Frame);
        for (uint16_t i = 0; i < DATA_SIZE; i++)
            for (int bit = 0; bit < 8; bit++)
            {
                uint8_t flipped[DATA_SIZE];
                std::memcpy(flipped, Frame, DATA_SIZE);
                flipped[i] ^= (uint8_t)(1u << bit);
                CHECK_EQ(Decode(flipped, DATA_SIZE), INVALID);
            }
    }

    // --- structural rejections ---------------------------------------------
    {
        uint8_t Frame[DATA_SIZE];
        uint8_t PayloadBuf[PAYLOAD_SIZE] = {0};
        EncodeWrite(0, PayloadBuf, Frame);

        uint8_t bad[DATA_SIZE];
        std::memcpy(bad, Frame, DATA_SIZE);
        bad[1] |= 0x20u; // non-zero padding (bit 5 is the lowest padding bit now)
        CHECK_EQ(Decode(bad, DATA_SIZE), INVALID);

        std::memcpy(bad, Frame, DATA_SIZE);
        bad[0] = 0x00; // missing start
        CHECK_EQ(Decode(bad, DATA_SIZE), INVALID);

        std::memcpy(bad, Frame, DATA_SIZE);
        bad[DATA_SIZE - 1] = 0x00; // missing end
        CHECK_EQ(Decode(bad, DATA_SIZE), INVALID);

        CHECK_EQ(Decode(Frame, READ_REQ_SIZE), INVALID); // wrong length for the command
        CHECK_EQ(Decode(Frame, 0), INVALID);

        // A write body claiming to be a digest request (9 bytes) must not decode: the length and
        // the command have to agree.
        std::memcpy(bad, Frame, DATA_SIZE);
        bad[1] = (uint8_t)((bad[1] & 0x0Fu) | CMD_DIGEST_REQ);
        CHECK_EQ(Decode(bad, DATA_SIZE), INVALID);
    }

    // Known-answer vectors, mirrored verbatim in the app's bootloader codec test.
    {
        uint8_t PayloadBuf[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) PayloadBuf[i] = (uint8_t)i;
        uint8_t Frame[DATA_SIZE];
        EncodeWrite(0, PayloadBuf, Frame);
        PrintHex("write0", Frame, DATA_SIZE);

        uint8_t rr[READ_REQ_SIZE];
        EncodeReadRequest(0x100, rr);
        PrintHex("readreq100", rr, READ_REQ_SIZE);

        for (int i = 0; i < PAYLOAD_SIZE; i++) PayloadBuf[i] = (uint8_t)(0xA0 + i);
        uint8_t rs[DATA_SIZE];
        EncodeReadResponse(0x40, PayloadBuf, rs);
        PrintHex("readresp40", rs, DATA_SIZE);

        uint8_t wr[READ_REQ_SIZE];
        EncodeWriteResponse(0x1040, wr);
        PrintHex("writeresp1040", wr, READ_REQ_SIZE);

        uint8_t ir[READ_REQ_SIZE];
        EncodeIdentifyRequest(ir);
        PrintHex("identifyreq", ir, READ_REQ_SIZE);

        for (int i = 0; i < PAYLOAD_SIZE; i++) PayloadBuf[i] = 0;
        uint8_t id[IDENTIFY_RESP_SIZE];
        EncodeIdentifyResponse(0x3600, 64, "DAS v0.1", id);
        PrintHex("identifyresp", id, IDENTIFY_RESP_SIZE);

        uint8_t er[ERASE_REQ_SIZE];
        EncodeEraseRequest(0x3440, er);
        PrintHex("erasereq3440", er, ERASE_REQ_SIZE);
    }

    // --- the echo window's three outcomes (card t_d9feed1d) -------------------
    // A raw send verifies its frame by the echo, but the node's answer legitimately starts
    // inside the LAST chunk's echo window (~704 us after our last byte: its own 32-byte silence
    // wait). Reading chunk-wise and memcmp-ing made that answer look like a collision and threw
    // it away, so read/write could never complete. Each byte is classified instead.
    {
        // Our own byte back: keep verifying, byte for byte, including a byte that happens to be
        // the start marker (a request's first byte IS 0xCA).
        CHECK(ClassifyEchoByte(0xCA, 0xCA) == EchoByte::Echo);
        CHECK(ClassifyEchoByte(0xAA, 0xAA) == EchoByte::Echo);
        CHECK(ClassifyEchoByte(0x00, 0x00) == EchoByte::Echo);

        // The answer beginning where our echo was expected: the expected case for read/write.
        // Every start-marker arrival is a reply whatever we had expected in that slot.
        CHECK(ClassifyEchoByte(0xBC, START) == EchoByte::Reply);
        CHECK(ClassifyEchoByte(0x00, START) == EchoByte::Reply);
        CHECK(ClassifyEchoByte(0xFF, START) == EchoByte::Reply);

        // Neither our echo nor the start marker: a real collision, caught mid-frame.
        CHECK(ClassifyEchoByte(0x00, 0xAA) == EchoByte::Collision);
        CHECK(ClassifyEchoByte(0xCA, 0x00) == EchoByte::Collision);
        CHECK(ClassifyEchoByte(0xBC, 0xBC) == EchoByte::Echo); // the END marker echoing is ours
    }

    // --- the folded parity is bit-identical to the per-byte ladder (size diet) -----
    // Core/Functions/Bootloader.h used to fold every byte on its own and XOR the results. It now
    // XORs the body into one byte and folds once (the parity of a XOR is the XOR of the parities).
    // Parity is what makes a frame acceptable, so prove the rewrite over every byte value and
    // every command value, on every frame length the codec has.
    {
        auto LadderParity = [](const uint8_t *Frame, uint16_t Len) {
            uint8_t Parity = 0;
            uint8_t Cmd = (uint8_t)(Frame[1] & 0x0Fu);
            for (int bit = 0; bit < 4; bit++) Parity ^= (uint8_t)((Cmd >> bit) & 1u);
            for (uint16_t i = 2; i + 1 < Len; i++)
            {
                uint8_t Byte = Frame[i];
                Byte ^= (uint8_t)(Byte >> 4);
                Byte ^= (uint8_t)(Byte >> 2);
                Byte ^= (uint8_t)(Byte >> 1);
                Parity ^= (uint8_t)(Byte & 1u);
            }
            return (uint8_t)(Parity & 1u);
        };

        int Mismatches = 0;
        for (int Cmd = 0; Cmd < 16; Cmd++)
            for (int Value = 0; Value < 256; Value++)
            {
                uint8_t Frame[DATA_SIZE];
                for (uint16_t i = 0; i < DATA_SIZE; i++) Frame[i] = (uint8_t)(i * 7 + 3);
                Frame[0] = START;
                Frame[1] = (uint8_t)Cmd;
                for (uint16_t i = 2; i < DATA_SIZE - 1; i++) Frame[i] = (uint8_t)Value;
                Frame[DATA_SIZE - 1] = END;
                if (Parity(Frame, DATA_SIZE) != LadderParity(Frame, DATA_SIZE)) Mismatches++;
                if (Parity(Frame, READ_REQ_SIZE) != LadderParity(Frame, READ_REQ_SIZE)) Mismatches++;
                if (Parity(Frame, DIGEST_REQ_SIZE) != LadderParity(Frame, DIGEST_REQ_SIZE)) Mismatches++;
                if (Parity(Frame, DIGEST_RESP_SIZE) != LadderParity(Frame, DIGEST_RESP_SIZE)) Mismatches++;
                if (Parity(Frame, IDENTIFY_RESP_SIZE) != LadderParity(Frame, IDENTIFY_RESP_SIZE)) Mismatches++;
            }
        CHECK_EQ(Mismatches, 0);
        std::printf("parity: 16 commands x 256 byte values x 5 frame lengths, 0 mismatches\n");
    }

    // --- every defined command is a frame size; nothing undefined decodes ------
    {
        for (int Cmd = 0; Cmd < 10; Cmd++) CHECK(FrameSize((uint8_t)Cmd) != 0);
        CHECK_EQ(FrameSize(CMD_DIGEST_REQ), DIGEST_REQ_SIZE);
        CHECK_EQ(FrameSize(CMD_DIGEST_RESP), DIGEST_RESP_SIZE);
        CHECK_EQ(FrameSize(CMD_IDENTIFY_RESP), IDENTIFY_RESP_SIZE);
        CHECK_EQ(FrameSize(CMD_ERASE_REQ), ERASE_REQ_SIZE);
        CHECK_EQ(FrameSize(CMD_ERASE_RESP), ERASE_RESP_SIZE);
        for (int Cmd = 10; Cmd < 16; Cmd++) CHECK_EQ(FrameSize((uint8_t)Cmd), 0); // undefined
        CHECK_EQ(FrameSize(16), 0);
        CHECK_EQ(FrameSize(0xFF), 0);
    }

    // --- digest request/response layout --------------------------------------
    {
                // The CRC always covers the whole app space, so the request carries nothing: it is the
        // shortest frame the codec has - start | control | end, 3 bytes.
        uint8_t Req[DIGEST_REQ_SIZE];
        EncodeDigestRequest(Req);
        CHECK_EQ(sizeof(Req), 3);
        CHECK_EQ(DIGEST_REQ_SIZE, 3);
        CHECK_EQ(Req[0], START);
        CHECK_EQ(Req[2], END);
        CHECK_EQ(Req[1] & 0x0Fu, CMD_DIGEST_REQ);
        CHECK_EQ(Req[1] & 0xE0u, 0);
        CHECK_EQ((Req[1] >> 4) & 1u, RefParity(Req, DIGEST_REQ_SIZE)); // parity of the command alone
        CHECK_EQ(Decode(Req, DIGEST_REQ_SIZE), CMD_DIGEST_REQ);
        CHECK_EQ(Decode(Req, DIGEST_RESP_SIZE), INVALID); // wrong length for the command
        CHECK_EQ(Decode(Req, READ_REQ_SIZE), INVALID);
        CHECK_EQ(Decode(Req, 2), INVALID);                // shorter than the frame

        // The response is start | control | crc32 | end = 7 bytes; no offset is echoed.
        uint8_t Resp[DIGEST_RESP_SIZE];
        EncodeDigestResponse(0xDEADBEEFu, Resp);
        CHECK_EQ(sizeof(Resp), 7);
        CHECK_EQ(DIGEST_RESP_SIZE, 7);
        CHECK_EQ(Resp[0], START);
        CHECK_EQ(Resp[6], END);
        CHECK_EQ(Resp[1] & 0x0Fu, CMD_DIGEST_RESP);
        CHECK_EQ((Resp[1] >> 4) & 1u, RefParity(Resp, DIGEST_RESP_SIZE));
        CHECK_EQ(DigestCrc(Resp), 0xDEADBEEFu);
        CHECK_EQ(Resp[2], 0xEF);              // u32 little-endian, right after the control byte
        CHECK_EQ(Resp[5], 0xDE);
        CHECK_EQ(Decode(Resp, DIGEST_RESP_SIZE), CMD_DIGEST_RESP);
        CHECK_EQ(Decode(Resp, DIGEST_REQ_SIZE), INVALID);

        // Every single-bit flip of either frame must be rejected.
        uint8_t Bad[DIGEST_RESP_SIZE];
        for (uint16_t i = 0; i < DIGEST_RESP_SIZE; i++)
            for (int bit = 0; bit < 8; bit++)
            {
                std::memcpy(Bad, Resp, DIGEST_RESP_SIZE);
                Bad[i] ^= (uint8_t)(1u << bit);
                CHECK_EQ(Decode(Bad, DIGEST_RESP_SIZE), INVALID);
            }

        PrintHex("digestreq", Req, DIGEST_REQ_SIZE);
        PrintHex("digestresp", Resp, DIGEST_RESP_SIZE);
    }

    // --- the digest's CRC32 is the one zlib.crc32 computes ---------------------
    // The tool and the node must agree byte for byte, so the expected values here come from
    // Python's zlib.crc32 (CRC-32/ISO-HDLC: poly 0xEDB88320, init 0xFFFFFFFF, final XOR).
    {
        CHECK_EQ(Crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
        CHECK_EQ(Crc32((const uint8_t *)"", 0), 0x00000000u);
        const uint8_t Zeros[4] = {0x00, 0x00, 0x00, 0x00};
        CHECK_EQ(Crc32(Zeros, 4), 0x2144DF1Cu);
        const uint8_t Erased[4] = {0xFF, 0xFF, 0xFF, 0xFF}; // flash's erased value
        CHECK_EQ(Crc32(Erased, 4), 0xFFFFFFFFu);

        // Feeding range byte by byte through Crc32Step - what the node does, because it pads
        // past the end of the app region one byte at a time - equals the whole-buffer form.
        uint8_t Range[64];
        for (int i = 0; i < 64; i++) Range[i] = (uint8_t)i;
        uint32_t Crc = 0xFFFFFFFFu;
        for (int i = 0; i < 64; i++) Crc = Crc32Step(Crc, Range[i]);
        CHECK_EQ(Crc ^ 0xFFFFFFFFu, Crc32(Range, 64));
        CHECK_EQ(Crc ^ 0xFFFFFFFFu, 0x100ECE8Cu);
    }

    // --- the command table the built image carries ----------------------------
    // Prints the control byte of one real frame per command, decoded back to operation and
    // direction. This is the table to compare against Docs/Services/Bootloader.md.
    {
        struct Row
        {
            const char *Name;
            uint8_t Byte;
            uint16_t Len;
        };
        uint8_t Buf[DATA_SIZE];
        uint8_t Zero[PAYLOAD_SIZE] = {0};
        auto Put = [&](const char *Name, uint16_t Len) { return Row{Name, Buf[1], Len}; };

        EncodeIdentifyRequest(Buf);
        Row Rows[10];
        Rows[0] = Put("identify-req ", READ_REQ_SIZE);
        EncodeIdentifyResponse(0x3600u, 64, "DAS v0.1", Buf);
        Rows[1] = Put("identify-resp", IDENTIFY_RESP_SIZE);
        EncodeReadRequest(0x100u, Buf);
        Rows[2] = Put("read-req     ", READ_REQ_SIZE);
        EncodeReadResponse(0x40u, Zero, Buf);
        Rows[3] = Put("read-resp    ", DATA_SIZE);
        EncodeEraseRequest(0x3440u, Buf);
        Rows[4] = Put("erase-req    ", ERASE_REQ_SIZE);
        EncodeEraseResponse(0x3440u, Buf);
        Rows[5] = Put("erase-resp   ", ERASE_RESP_SIZE);
        EncodeWrite(0, Zero, Buf);
        Rows[6] = Put("write        ", DATA_SIZE);
        EncodeWriteResponse(0x1040u, Buf);
        Rows[7] = Put("write-resp   ", READ_REQ_SIZE);
        EncodeDigestRequest(Buf);
        Rows[8] = Put("digest-req   ", DIGEST_REQ_SIZE);
        EncodeDigestResponse(0xDEADBEEFu, Buf);
        Rows[9] = Put("digest-resp  ", DIGEST_RESP_SIZE);

        const uint8_t Expect[10] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9};
        std::printf("command table (control byte of a real frame):\n");
        std::printf("  frame          control  parity  cmd  op(bits1-3)  dir\n");
        for (int i = 0; i < 10; i++)
        {
            const uint8_t Ctl = Rows[i].Byte;
            const uint8_t Cmd = (uint8_t)(Ctl & 0x0Fu);
            CHECK_EQ(Cmd, Expect[i]);
            CHECK_EQ(FrameSize(Cmd), Rows[i].Len);
            std::printf("  %s  0x%02X     %u       %X     %u            %u\n", Rows[i].Name, Ctl,
                        (unsigned)((Ctl >> 4) & 1u), (unsigned)Cmd, (unsigned)(Cmd >> 1),
                        (unsigned)(Cmd & 1u));
        }
    }

    // --- a valid frame is not necessarily OUR answer (card t_c9a37bb8) ----------
    // The core's passthrough assembles the first valid 0xCA frame on the bus and forwards it as the
    // answer to its outstanding request. On a half-duplex line the core also hears its OWN raw
    // request echo, and for the header-only requests that echo IS a valid 7-byte frame - the rig
    // measured a differing chunk reading the END marker 0xBC at byte 0, which is PtRx+6 of exactly
    // such a frame. Two predicates let the device code reject it: the response command the CID
    // expects, and the offset a read response has to echo.
    {
        CHECK_EQ(PassthroughResponse(0x20), CMD_IDENTIFY_RESP);
        CHECK_EQ(PassthroughResponse(0x21), CMD_READ_RESP);
        CHECK_EQ(PassthroughResponse(0x22), CMD_ERASE_RESP);
        CHECK_EQ(PassthroughResponse(0x23), CMD_WRITE_RESP);
        CHECK_EQ(PassthroughResponse(0x24), CMD_DIGEST_RESP);
        CHECK_EQ(PassthroughResponse(0x1F), INVALID); // outside the passthrough CID range
        CHECK_EQ(PassthroughResponse(0x25), INVALID);
        // Every mapping is a RESPONSE (bit 0 set) with a real frame size - a request mapped here
        // would accept the core's own echo, which is the defect itself.
        for (uint8_t Cid = 0x20; Cid <= 0x24; Cid++)
        {
            const uint8_t Resp = PassthroughResponse(Cid);
            CHECK((Resp & 1u) == 1u);
            CHECK(FrameSize(Resp) != 0);
        }

        // The core's own read request is the frame the route used to accept: 7 bytes, command
        // 0b0010, offset echoed, END marker last. It must NOT be this CID's response.
        uint8_t Req[READ_REQ_SIZE];
        EncodeReadRequest(0x1200u, Req);
        CHECK_EQ((uint8_t)(Req[1] & 0x0Fu), CMD_READ_REQ);
        CHECK((Req[1] & 0x0Fu) != PassthroughResponse(0x21));
        // ... and the END marker really is the byte the core forwarded as payload byte 0, which is
        // the "reads 0xbc at byte 0" signature: PtRx + HEADER_SIZE of a 7-byte frame.
        CHECK_EQ(Req[HEADER_SIZE], END);

        // A read response answers THIS read only when it echoes the offset asked for.
        uint8_t Resp[DATA_SIZE];
        uint8_t Body[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) Body[i] = (uint8_t)(0xA0 + i);
        EncodeReadResponse(0x1200u, Body, Resp);
        CHECK(ReadAnswersOffset(Resp, 0x1200u));
        CHECK(!ReadAnswersOffset(Resp, 0x1220u)); // the next chunk's answer, arriving late
        CHECK(!ReadAnswersOffset(Resp, 0x0000u));
    }

    std::printf("== bootloader packet tests (v2 codec): %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
