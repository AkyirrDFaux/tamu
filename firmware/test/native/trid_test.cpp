// Host-side test for NextSystemTrid (Core/Functions/Packet.h) - the System/Logs TRID
// allocator.
//
// Docs/RSBus and Packets.md: the System/Log range is 0x0000-0x0FFF and is a single
// incrementing counter. The high byte carries the service type so an echoed reply still
// routes to the service; the low byte is the counter, which wraps at 256. This pins both
// the range and the increment/wrap behaviour without a device.
#include <cstdint>
#include <cstdio>

#include "Blocks/DeviceInfo.h" // DeviceStatusStruct, which Packet.h forward-uses
#include "Core/Functions/Packet.h"

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
    // Every System/Log TRID stays inside 0x0000-0x0FFF and carries the service type in the
    // high byte (so a reply on the same TRID routes back to where the request came from).
    const ServiceType types[] = {ServiceType::Device, ServiceType::Register,
                                 ServiceType::LogHandler, ServiceType::Storage,
                                 ServiceType::Subscriptions, ServiceType::Script};
    for (ServiceType t : types) {
        const uint16_t trid = NextSystemTrid(t);
        CHECK_EQ(trid >> 8, (unsigned)t);
        CHECK_EQ(GetServiceType(trid), t);
        CHECK((trid & 0xF000) == 0); // within the documented 0x0FFF range
    }

    // One shared counter: 256 successive allocations visit every low byte exactly once,
    // then wrap back to where they started. The counter's phase is arbitrary (earlier tests
    // or translation units may have advanced it), so compare against the first sample.
    const uint16_t first = NextSystemTrid(ServiceType::Device);
    const uint8_t firstLow = (uint8_t)(first & 0xFF);
    bool seen[256] = {false};
    seen[firstLow] = true;
    for (int i = 1; i < 256; i++) {
        const uint16_t v = NextSystemTrid(ServiceType::Device);
        const uint8_t low = (uint8_t)(v & 0xFF);
        CHECK_EQ(low, (uint8_t)(firstLow + i)); // increments by one, wraps
        CHECK_EQ(v >> 8, (unsigned)ServiceType::Device);
        CHECK(!seen[low]); // each low byte exactly once
        seen[low] = true;
    }
    const uint16_t after = NextSystemTrid(ServiceType::Device);
    CHECK_EQ((uint8_t)(after & 0xFF), firstLow); // wrapped
    CHECK_EQ(after >> 8, (unsigned)ServiceType::Device);

    // The counter is shared across service types: switching type still advances it by one.
    const uint8_t before = (uint8_t)(NextSystemTrid(ServiceType::Register) & 0xFF);
    const uint8_t cross = (uint8_t)(NextSystemTrid(ServiceType::LogHandler) & 0xFF);
    CHECK_EQ(cross, (uint8_t)(before + 1));

    std::printf("== trid tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
