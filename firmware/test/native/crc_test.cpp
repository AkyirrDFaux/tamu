// Host-side test for Crc8 (Core/Functions/Packet.h) - the wire checksum.
//
// OPTIMIZE_SPEED gives Crc8 two shapes: a 256-byte lookup table (speed) and an eight-iteration
// bit loop (size). run.sh builds this file both ways and every assertion here must hold for
// both, which is what keeps the table from drifting away from the loop.
//
// The reference implementation below is deliberately independent: it does *not* share the
// firmware's step function, it keeps the CRC in a shift register with the documented
// parameters (poly 0x07, init 0x00, no reflection, no final xor) from Specs/Data Formats.md.
// That also pins the parameters themselves, not just the equivalence of the two shapes.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/Crc8.h"

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

// Independent reference: polynomial long division, MSB first.
static uint8_t RefCrc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (uint8_t)((crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1));
    }
    return crc;
}

int main()
{
    // The standard check value for CRC-8/ATM: "123456789" -> 0xF4.
    const char *check = "123456789";
    CHECK_EQ(Crc8((const uint8_t *)check, 9), 0xF4);
    CHECK_EQ(RefCrc8((const uint8_t *)check, 9), 0xF4);

    // Empty input leaves the init value.
    CHECK_EQ(Crc8(nullptr, 0), 0x00);

    // Every byte value in isolation, then every value as a prefix to a fixed tail: that covers
    // all 256 table entries both as a starting state and as an indexed byte.
    for (int b = 0; b < 256; b++)
    {
        const uint8_t one[1] = {(uint8_t)b};
        CHECK_EQ(Crc8(one, 1), RefCrc8(one, 1));
    }

    // A pseudo-random sweep over lengths (including the maximum frame length) with a simple
    // LCG, so the comparison covers realistic packet bodies rather than hand-picked ones.
    uint8_t buf[128];
    uint32_t lcg = 0x12345678u;
    for (int len = 0; len <= (int)sizeof(buf); len++)
    {
        for (int i = 0; i < len; i++)
        {
            lcg = lcg * 1664525u + 1013904223u;
            buf[i] = (uint8_t)(lcg >> 24);
        }
        CHECK_EQ(Crc8(buf, (uint16_t)len), RefCrc8(buf, (size_t)len));
    }

    // The largest length the wire format allows (MAX_PAYLOAD_SIZE - 1 + header bytes).
    {
        uint8_t big[256];
        for (int i = 0; i < 256; i++) big[i] = (uint8_t)(i * 7 + 3);
        CHECK_EQ(Crc8(big, 128), RefCrc8(big, 128));
        CHECK_EQ(Crc8(big, 256), RefCrc8(big, 256));
    }

    // A checksum that detects a single flipped bit anywhere in a frame.
    {
        const uint8_t frame[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        const uint8_t want = Crc8(frame, sizeof(frame));
        for (size_t i = 0; i < sizeof(frame); i++)
            for (int bit = 0; bit < 8; bit++)
            {
                uint8_t flipped[12];
                std::memcpy(flipped, frame, sizeof(frame));
                flipped[i] ^= (uint8_t)(1u << bit);
                CHECK(Crc8(flipped, sizeof(flipped)) != want);
            }
    }

#ifdef OPTIMIZE_SPEED
    std::printf("== crc8 tests (OPTIMIZE_SPEED: table): %d checks, %d failures ==\n", checks, failures);
#else
    std::printf("== crc8 tests (size: bit loop): %d checks, %d failures ==\n", checks, failures);
#endif
    return failures == 0 ? 0 : 1;
}
