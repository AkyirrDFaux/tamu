// Host-side test for the LogMessage layout (Core/Types/Log.h).
//
// Docs/Services/Log Handler.md: the Log Struct is Source(16: BlockType+Instance) |
// Category(8) | Specifics(8) | Timestamp(32). A service-originated log uses the reserved
// type 0x3FF with the ServiceType in the instance field, so the full 16-bit source stays
// available for a block type + instance. This pins the encoding and the 8-byte wire size.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Types/Log.h"

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
            std::printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__,    \
                        #got, g, w);                                                 \
        }                                                                            \
    } while (0)

int main() {
    CHECK_EQ(sizeof(LogMessage), 8);

    // Block log: the block type (10 bits) and instance (6 bits) both survive.
    const LogMessage b = MakeLog(true, 0x05 /* AccGyr */, 0x1234, 42, 7);
    CHECK_EQ(b.source, 0x05 | (7 << 10));
    CHECK(LogIsBlock(b));
    CHECK_EQ(LogSourceType(b), 0x05);
    CHECK_EQ(LogSourceInstance(b), 7);
    CHECK_EQ(LogSourceId(b), 0x05); // a block source id is the block type
    CHECK_EQ(b.category, 0x12);
    CHECK_EQ(b.specifics, 0x34);
    CHECK_EQ(LogCode(b), 0x1234);
    CHECK_EQ(b.timestamp, 42);

    // Service log: the reserved type + the ServiceType in the instance field.
    const LogMessage s = MakeLog(false, 0x03 /* Storage */, 0x00AB, 9);
    CHECK_EQ(LogSourceType(s), LOG_SOURCE_SERVICE);
    CHECK(!LogIsBlock(s));
    CHECK_EQ(LogSourceInstance(s), 0x03);
    CHECK_EQ(LogSourceId(s), 0x03);
    CHECK_EQ(LogCode(s), 0x00AB);
    CHECK_EQ(s.timestamp, 9);

    // The wire order is source(LE u16) | category | specifics | timestamp (does not depend on
    // the host's native layout because the struct is packed and written/read as bytes).
    uint8_t bytes[8];
    std::memcpy(bytes, &b, sizeof(bytes));
    CHECK_EQ(bytes[0], 0x05);
    CHECK_EQ(bytes[1], 0x1C);
    CHECK_EQ(bytes[2], 0x12);
    CHECK_EQ(bytes[3], 0x34);

    std::printf("== log tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
