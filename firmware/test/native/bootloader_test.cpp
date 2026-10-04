// Host-side test for the bootloader packet codec (Core/Functions/Bootloader.h).
//
// The codec is the contract between the app and the bootloader (Docs/Services/Bootloader.md),
// so this pins the byte layout, the even parity and the frame sizes. The parity reference below
// is deliberately independent (a plain bit walk) so a broken fold in the codec cannot agree
// with itself. The KAT hex strings at the end are mirrored verbatim in the app test.

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

// Independent even parity over the "rest of the packet": the command (byte 1, bits 0-1), the
// offset and the payload (bytes 2..len-2). The markers and the padding bits are excluded.
static uint8_t RefParity(const uint8_t *f, uint16_t len)
{
    uint8_t p = 0;
    for (int bit = 0; bit < 2; bit++)
        p ^= (uint8_t)((f[1] >> bit) & 1u);
    for (uint16_t i = 2; i + 1 < len; i++)
        for (int bit = 0; bit < 8; bit++)
            p ^= (uint8_t)((f[i] >> bit) & 1u);
    return (uint8_t)(p & 1u);
}

static void PrintHex(const char *label, const uint8_t *f, uint16_t len)
{
    std::printf("KAT %s = ", label);
    for (uint16_t i = 0; i < len; i++) std::printf("%02X", f[i]);
    std::printf("\n");
}

int main()
{
    using namespace Bootloader;

    // --- write frame layout -------------------------------------------------
    {
        uint8_t payload[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) payload[i] = (uint8_t)i;
        uint8_t f[DATA_SIZE];
        EncodeWrite(0x00000000u, payload, f);

        CHECK_EQ(sizeof(f), 39);
        CHECK_EQ(f[0], START);
        CHECK_EQ(f[38], END);
        CHECK_EQ(f[1] & 0x03u, CMD_WRITE);
        CHECK_EQ(f[1] & 0xF8u, 0);            // padding zero
        CHECK_EQ((f[1] >> 2) & 1u, RefParity(f, DATA_SIZE));
        CHECK_EQ(Offset(f), 0u);
        CHECK(std::memcmp(Payload(f), payload, PAYLOAD_SIZE) == 0);
        CHECK_EQ(Decode(f, DATA_SIZE), CMD_WRITE);
    }

    // --- read-request layout ------------------------------------------------
    {
        uint8_t f[READ_REQ_SIZE];
        EncodeReadRequest(0x12345678u, f);
        CHECK_EQ(sizeof(f), 7);
        CHECK_EQ(f[0], START);
        CHECK_EQ(f[6], END);
        CHECK_EQ(f[1] & 0x03u, CMD_READ_REQ);
        CHECK_EQ((f[1] >> 2) & 1u, RefParity(f, READ_REQ_SIZE));
        CHECK_EQ(Offset(f), 0x12345678u);
        CHECK_EQ(Decode(f, READ_REQ_SIZE), CMD_READ_REQ);
    }

    // --- read-response layout ----------------------------------------------
    {
        uint8_t payload[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) payload[i] = (uint8_t)(0xFF - i);
        uint8_t f[DATA_SIZE];
        EncodeReadResponse(0x00000040u, payload, f);
        CHECK_EQ(f[0], START);
        CHECK_EQ(f[38], END);
        CHECK_EQ(f[1] & 0x03u, CMD_READ_RESP);
        CHECK_EQ((f[1] >> 2) & 1u, RefParity(f, DATA_SIZE));
        CHECK_EQ(Offset(f), 0x00000040u);
        CHECK(std::memcmp(Payload(f), payload, PAYLOAD_SIZE) == 0);
        CHECK_EQ(Decode(f, DATA_SIZE), CMD_READ_RESP);
    }

    // --- every single-bit flip must be rejected -----------------------------
    {
        uint8_t payload[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) payload[i] = (uint8_t)(i * 3 + 1);
        uint8_t f[DATA_SIZE];
        EncodeWrite(0x00000100u, payload, f);
        for (uint16_t i = 0; i < DATA_SIZE; i++)
            for (int bit = 0; bit < 8; bit++)
            {
                uint8_t flipped[DATA_SIZE];
                std::memcpy(flipped, f, DATA_SIZE);
                flipped[i] ^= (uint8_t)(1u << bit);
                CHECK_EQ(Decode(flipped, DATA_SIZE), 0);
            }
    }

    // --- structural rejections ---------------------------------------------
    {
        uint8_t f[DATA_SIZE];
        uint8_t payload[PAYLOAD_SIZE] = {0};
        EncodeWrite(0, payload, f);

        uint8_t bad[DATA_SIZE];
        std::memcpy(bad, f, DATA_SIZE);
        bad[1] |= 0x08u; // non-zero padding
        CHECK_EQ(Decode(bad, DATA_SIZE), 0);

        std::memcpy(bad, f, DATA_SIZE);
        bad[0] = 0x00; // missing start
        CHECK_EQ(Decode(bad, DATA_SIZE), 0);

        std::memcpy(bad, f, DATA_SIZE);
        bad[DATA_SIZE - 1] = 0x00; // missing end
        CHECK_EQ(Decode(bad, DATA_SIZE), 0);

        CHECK_EQ(Decode(f, READ_REQ_SIZE), 0); // wrong length for the command
        CHECK_EQ(Decode(f, 0), 0);
    }

    // Known-answer vectors, mirrored verbatim in the app's bootloader codec test.
    {
        uint8_t payload[PAYLOAD_SIZE];
        for (int i = 0; i < PAYLOAD_SIZE; i++) payload[i] = (uint8_t)i;
        uint8_t w[DATA_SIZE];
        EncodeWrite(0, payload, w);
        PrintHex("write0", w, DATA_SIZE);

        uint8_t rr[READ_REQ_SIZE];
        EncodeReadRequest(0x100, rr);
        PrintHex("readreq100", rr, READ_REQ_SIZE);

        for (int i = 0; i < PAYLOAD_SIZE; i++) payload[i] = (uint8_t)(0xA0 + i);
        uint8_t rs[DATA_SIZE];
        EncodeReadResponse(0x40, payload, rs);
        PrintHex("readresp40", rs, DATA_SIZE);
    }

    std::printf("== bootloader packet tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
