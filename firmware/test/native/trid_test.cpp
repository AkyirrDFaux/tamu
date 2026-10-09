// Host-side test for NextSystemTrid (Core/Functions/Packet.h) - the System/Logs TRID
// allocator.
//
// Docs/RSBus and Packets.md: the System/Log range is 0x0000-0x0FFF and is a single
// incrementing counter. B32 removed the service tag from the high byte, so a plain 12-bit
// counter (0x0000-0x0FFF) wraps at the range end regardless of which service asks. This
// pins the range and the increment/wrap behaviour without a device.
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
    // Every System/Log TRID stays inside 0x0000-0x0FFF; the high nibble no longer encodes a
    // service type.
    const ServiceType types[] = {ServiceType::Device, ServiceType::Register,
                                 ServiceType::LogHandler, ServiceType::Storage,
                                 ServiceType::Subscriptions, ServiceType::Script};
    for (ServiceType t : types) {
        const uint16_t trid = NextSystemTrid(t);
        CHECK((trid & 0xF000) == 0); // within the documented 0x0FFF range
    }

    // One shared counter: every successive allocation increments by one and wraps from
    // 0x0FFF back to 0x0000. The counter's phase is arbitrary (earlier tests or translation
    // units may have advanced it), so compare against the previous sample rather than a fixed
    // start. 2*0x1000 steps guarantee at least one wrap.
    const uint16_t first = NextSystemTrid(ServiceType::Device);
    CHECK((first & 0xF000) == 0);
    uint16_t prev = first;
    int wraps = 0;
    for (int i = 0; i < 0x2000; i++) {
        const uint16_t v = NextSystemTrid(ServiceType::Register);
        CHECK_EQ(v, (uint16_t)((prev + 1) & TRID_SYSTEM_MAX));
        CHECK((v & 0xF000) == 0);
        if (v == 0) wraps++;
        prev = v;
    }
    CHECK(wraps >= 1); // the counter wrapped inside the sampled span

    // The counter is shared across service types: switching type still advances it by one and
    // the type argument is otherwise ignored.
    const uint16_t before = NextSystemTrid(ServiceType::Device);
    const uint16_t cross = NextSystemTrid(ServiceType::LogHandler);
    CHECK_EQ(cross, (uint16_t)((before + 1) & TRID_SYSTEM_MAX));

    std::printf("== trid tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
