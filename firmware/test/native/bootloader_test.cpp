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
    CHECK_EQ(CMD_WRITE & 0x0Fu, 0x2u);
    CHECK_EQ(CMD_ERASE_REQ, 0x8u);
    CHECK_EQ(CMD_ERASE_RESP, 0x9u);
    CHECK_EQ(IDENTIFY_RESP_SIZE, 41);
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
        EncodeEraseRequest(0x00003440u, Req);          // any offset inside the page
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

    // --- identify: request header-only, response carries size + page + name ----
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
        CHECK_EQ(sizeof(resp), 41);
        CHECK_EQ(resp[0], START);
        CHECK_EQ(resp[40], END);
        CHECK_EQ(resp[1] & 0x0Fu, CMD_IDENTIFY_RESP);
        CHECK_EQ(resp[1] & 0xE0u, 0);
        CHECK_EQ((resp[1] >> 4) & 1u, RefParity(resp, IDENTIFY_RESP_SIZE));
        CHECK_EQ(Offset(resp), 0x00003600u);
        CHECK_EQ(IdentifyPageSize(resp), 64);          // the erase unit, u16 little-endian
        CHECK_EQ(resp[6], 64);
        CHECK_EQ(resp[7], 0);
        CHECK(std::memcmp(IdentifyName(resp), "DAS v0.1", 9) == 0);
        CHECK_EQ(IdentifyName(resp)[9], 0);            // null-padded
        CHECK_EQ(IdentifyName(resp)[NAME_SIZE - 1], 0);
        CHECK_EQ(Decode(resp, IDENTIFY_RESP_SIZE), CMD_IDENTIFY_RESP);
        // The v1 length must NOT decode: the length is how a tool tells v1 from v2.
        CHECK_EQ(Decode(resp, DATA_SIZE), INVALID);
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

        // A write body claiming to be a digest request (9 bytes) must not decode.
        std::memcpy(bad, Frame, DATA_SIZE);
        bad[1] = (uint8_t)((bad[1] & 0x0Fu) | 0x06u); // digest-request command
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
        uint8_t Req[DIGEST_REQ_SIZE];
        EncodeDigestRequest(0x00001200u, 0x1000u, Req);
        CHECK_EQ(sizeof(Req), 9);
        CHECK_EQ(Req[0], START);
        CHECK_EQ(Req[8], END);
        CHECK_EQ(Req[1] & 0x0Fu, CMD_DIGEST_REQ);
        CHECK_EQ(Req[1] & 0xE0u, 0);
        CHECK_EQ((Req[1] >> 4) & 1u, RefParity(Req, DIGEST_REQ_SIZE));
        CHECK_EQ(Offset(Req), 0x00001200u);   // the offset keeps the header's own slot
        CHECK_EQ(DigestLength(Req), 0x1000u);
        CHECK_EQ(Req[6], 0x00);               // u16 little-endian
        CHECK_EQ(Req[7], 0x10);
        CHECK_EQ(Decode(Req, DIGEST_REQ_SIZE), CMD_DIGEST_REQ);
        CHECK_EQ(Decode(Req, DIGEST_RESP_SIZE), INVALID); // wrong length for the command
        CHECK_EQ(Decode(Req, READ_REQ_SIZE), INVALID);

        uint8_t Resp[DIGEST_RESP_SIZE];
        EncodeDigestResponse(0x00001200u, 0xDEADBEEFu, Resp);
        CHECK_EQ(sizeof(Resp), 11);
        CHECK_EQ(Resp[0], START);
        CHECK_EQ(Resp[10], END);
        CHECK_EQ(Resp[1] & 0x0Fu, CMD_DIGEST_RESP);
        CHECK_EQ((Resp[1] >> 4) & 1u, RefParity(Resp, DIGEST_RESP_SIZE));
        CHECK_EQ(Offset(Resp), 0x00001200u);  // offset echoed
        CHECK_EQ(DigestCrc(Resp), 0xDEADBEEFu);
        CHECK_EQ(Resp[6], 0xEF);              // u32 little-endian
        CHECK_EQ(Resp[9], 0xDE);
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

    std::printf("== bootloader packet tests (v2 codec): %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
