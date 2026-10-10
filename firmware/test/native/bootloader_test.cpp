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

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s = %lld, want 0x%llX\n", __FILE__, __LINE__,  \
                        #got, g, w);                                                 \
        }                                                                            \
    } while (0)

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

        std::memcpy(bad, Frame, DATA_SIZE);
        bad[1] = (uint8_t)((bad[1] & 0x07u) | 0x06u); // undefined command 0b110
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

    std::printf("== bootloader packet tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
