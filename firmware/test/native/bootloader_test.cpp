// Host-side test for the bootloader packet codec (Core/Functions/Bootloader.h).
//
// The codec is the contract between the app and the bootloader (Docs/Services/Bootloader.md),
// so this pins the byte layout, the even parity and the frame sizes. The parity reference below
// is deliberately independent (a plain bit walk) so a broken fold in the codec cannot agree
// with itself. The KAT hex strings at the end are mirrored verbatim in the app test.
//
// Protocol v2: the control byte is padding(4)=0 | parity(1) | command(3), and Decode's
// "invalid" sentinel is 0xFF (0b000 is a valid command: identify-request).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/Bootloader.h"

#include "check.h"

// Independent even parity over the "rest of the packet": the command (byte 1, bits 0-2), the
// offset and the payload (bytes 2..len-2). The markers and the padding bits are excluded.
static uint8_t RefParity(const uint8_t *Frame, uint16_t len)
{
    uint8_t Parity = 0;
    for (int bit = 0; bit < 3; bit++)
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

    // --- write frame layout -------------------------------------------------
    {
        uint8_t PayloadBuf[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) PayloadBuf[i] = (uint8_t)i;
        uint8_t Frame[DATA_SIZE];
        EncodeWrite(0x00000000u, PayloadBuf, Frame);

        CHECK_EQ(sizeof(Frame), 39);
        CHECK_EQ(Frame[0], START);
        CHECK_EQ(Frame[38], END);
        CHECK_EQ(Frame[1] & 0x07u, CMD_WRITE);
        CHECK_EQ(Frame[1] & 0xF0u, 0);            // padding zero
        CHECK_EQ((Frame[1] >> 3) & 1u, RefParity(Frame, DATA_SIZE));
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
        CHECK_EQ(Frame[1] & 0x07u, CMD_READ_REQ);
        CHECK_EQ((Frame[1] >> 3) & 1u, RefParity(Frame, READ_REQ_SIZE));
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
        CHECK_EQ(Frame[1] & 0x07u, CMD_READ_RESP);
        CHECK_EQ((Frame[1] >> 3) & 1u, RefParity(Frame, DATA_SIZE));
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
        CHECK_EQ(Frame[1] & 0x07u, CMD_WRITE_RESP);
        CHECK_EQ((Frame[1] >> 3) & 1u, RefParity(Frame, READ_REQ_SIZE));
        CHECK_EQ(Offset(Frame), 0x00001040u);
        CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_WRITE_RESP);
    }

    // --- identify: request is header-only, response carries size + name -------
    {
        uint8_t req[READ_REQ_SIZE];
        EncodeIdentifyRequest(req);
        CHECK_EQ(req[0], START);
        CHECK_EQ(req[6], END);
        CHECK_EQ(req[1] & 0x07u, CMD_IDENTIFY_REQ);
        CHECK_EQ(Offset(req), 0u);
        // 0b000 is a valid command: Decode must NOT report it as invalid.
        CHECK_EQ(Decode(req, READ_REQ_SIZE), CMD_IDENTIFY_REQ);

        uint8_t resp[DATA_SIZE];
        EncodeIdentifyResponse(0x00003600u, "DAS v0.1", resp);
        CHECK_EQ(resp[0], START);
        CHECK_EQ(resp[38], END);
        CHECK_EQ(resp[1] & 0x07u, CMD_IDENTIFY_RESP);
        CHECK_EQ((resp[1] >> 3) & 1u, RefParity(resp, DATA_SIZE));
        CHECK_EQ(Offset(resp), 0x00003600u);
        CHECK(std::memcmp(Payload(resp), "DAS v0.1", 9) == 0);
        CHECK_EQ(Payload(resp)[9], 0);              // null-padded
        CHECK_EQ(Payload(resp)[NAME_SIZE - 1], 0);
        CHECK_EQ(Decode(resp, DATA_SIZE), CMD_IDENTIFY_RESP);
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
        bad[1] |= 0x10u; // non-zero padding (bit 4 is now padding)
        CHECK_EQ(Decode(bad, DATA_SIZE), INVALID);

        std::memcpy(bad, Frame, DATA_SIZE);
        bad[0] = 0x00; // missing start
        CHECK_EQ(Decode(bad, DATA_SIZE), INVALID);

        std::memcpy(bad, Frame, DATA_SIZE);
        bad[DATA_SIZE - 1] = 0x00; // missing end
        CHECK_EQ(Decode(bad, DATA_SIZE), INVALID);

        CHECK_EQ(Decode(Frame, READ_REQ_SIZE), INVALID); // wrong length for the command
        CHECK_EQ(Decode(Frame, 0), INVALID);

        // The 3-bit command field is now fully used (0b110/0b111 are the digest pair), so an
        // undefined command no longer exists: what rejects a mislabelled frame is the length it
        // announces. A write body claiming to be a digest request (9 bytes) must not decode.
        std::memcpy(bad, Frame, DATA_SIZE);
        bad[1] = (uint8_t)((bad[1] & 0x07u) | 0x06u); // digest-request command
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
        uint8_t id[DATA_SIZE];
        EncodeIdentifyResponse(0x3600, "DAS v0.1", id);
        PrintHex("identifyresp", id, DATA_SIZE);
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
    // every command value, on both a full data frame and a header-only frame.
    {
        auto LadderParity = [](const uint8_t *Frame, uint16_t Len) {
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
        };

        int Mismatches = 0;
        for (int Cmd = 0; Cmd < 8; Cmd++)
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
            }
        CHECK_EQ(Mismatches, 0);
        std::printf("parity: 8 commands x 256 byte values x 4 frame lengths, 0 mismatches\n");
    }

    // --- every command in the 3-bit field is a frame size; nothing is undefined ---
    {
        for (int Cmd = 0; Cmd < 8; Cmd++) CHECK(FrameSize((uint8_t)Cmd) != 0);
        CHECK_EQ(FrameSize(CMD_DIGEST_REQ), DIGEST_REQ_SIZE);
        CHECK_EQ(FrameSize(CMD_DIGEST_RESP), DIGEST_RESP_SIZE);
        CHECK_EQ(FrameSize(8), 0); // out of the 3-bit field cannot reach a table entry
    }

    // --- digest request/response layout --------------------------------------
    {
        uint8_t Req[DIGEST_REQ_SIZE];
        EncodeDigestRequest(0x00001200u, 0x1000u, Req);
        CHECK_EQ(sizeof(Req), 9);
        CHECK_EQ(Req[0], START);
        CHECK_EQ(Req[8], END);
        CHECK_EQ(Req[1] & 0x07u, CMD_DIGEST_REQ);
        CHECK_EQ(Req[1] & 0xF0u, 0);
        CHECK_EQ((Req[1] >> 3) & 1u, RefParity(Req, DIGEST_REQ_SIZE));
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
        CHECK_EQ(Resp[1] & 0x07u, CMD_DIGEST_RESP);
        CHECK_EQ((Resp[1] >> 3) & 1u, RefParity(Resp, DIGEST_RESP_SIZE));
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

    std::printf("== bootloader packet tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
