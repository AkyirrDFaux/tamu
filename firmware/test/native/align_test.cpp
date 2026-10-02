// Verifies the alignment guarantees the wire and byte-buffer code relies on.
//
// The CH32V003 (RV32EC) raises a load/store fault on a misaligned word, unlike the ESP32's
// Xtensa. So `PacketFrame` must be 4-byte aligned (it is `packed`, which on its own would
// leave it 1-aligned) and every multi-byte access through a byte buffer must go through
// LoadUnaligned/StoreUnaligned. This is the host stand-in for "the node does not fault here".
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "Blocks/DeviceInfo.h" // DeviceStatusStruct, which Packet.h forward-uses
#include "Core/Functions/Packet.h"

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

struct Pod {
    uint16_t a;
    uint8_t b;
    uint8_t c;
};

int main() {
    // The whole typed-payload scheme rests on this.
    static_assert(alignof(PacketFrame) == 4, "PacketFrame must be 4-byte aligned");
    CHECK(alignof(PacketFrame) == 4);

    // Every declared frame - local, global, in an array - is word-aligned, so payload + 4k is
    // a real word boundary and a u16 at an even offset is fine too.
    PacketFrame local;
    PacketFrame arr[3];
    CHECK((uintptr_t)local.payload % 4 == 0);
    CHECK((uintptr_t)arr[0].payload % 4 == 0);
    CHECK((uintptr_t)arr[1].payload % 4 == 0);
    CHECK((uintptr_t)arr[2].payload % 4 == 0);
    CHECK(sizeof(PacketFrame) % 4 == 0);

    // A byte buffer with an odd base: access it at every offset and every phase.
    alignas(8) uint8_t storage[96];
    for (int phase = 0; phase < 4; phase++) {
        uint8_t *base = storage + phase; // phases 1..3 are deliberately misaligned
        for (int off = 0; off < 32; off++) {
            memset(storage, 0, sizeof(storage));

            uint32_t v32 = 0xA1B2C3D4u + (uint32_t)off * 0x01010101u;
            StoreUnaligned(base + off, v32);
            for (int b = 0; b < 4; b++)
                CHECK((base + off)[b] == (uint8_t)(v32 >> (8 * b))); // little-endian, exact spot
            CHECK(LoadUnaligned<uint32_t>(base + off) == v32);

            int32_t s32 = -1000000 - off * 7;
            StoreUnaligned(base + off, s32);
            CHECK(LoadUnaligned<int32_t>(base + off) == s32);

            uint16_t v16 = (uint16_t)(0x1234u + off);
            StoreUnaligned(base + off, v16);
            CHECK((base + off)[0] == (uint8_t)v16);
            CHECK((base + off)[1] == (uint8_t)(v16 >> 8));
            CHECK(LoadUnaligned<uint16_t>(base + off) == v16);
        }
    }

    // Struct round-trip at an odd address.
    alignas(8) uint8_t sbuf[8];
    for (int phase = 1; phase < 4; phase++) {
        Pod p = {0xBEEF, 0x12, 0x34};
        StoreUnaligned(sbuf + phase, p);
        Pod q = LoadUnaligned<Pod>(sbuf + phase);
        CHECK(q.a == p.a && q.b == p.b && q.c == p.c);
    }

    if (failures == 0)
        printf("align: all checks passed\n");
    return failures ? 1 : 0;
}
