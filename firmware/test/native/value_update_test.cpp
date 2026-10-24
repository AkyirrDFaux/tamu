// Host-side test for a zero-length value update against a dynamic block target
// (Core/Functions/MemoryBlocks.h, with the shared decision in Core/Functions/MemoryTypes.h).
//
// The defect this pins (bambi f6 death, card t_b579fa77): a value that arrives with no value bytes
// was applied as a 0-length value. SetEntry then stored Size = 0, GetKey resolved that key to
// DataPtr = nullptr for ever after, and every later update to the field was dropped on
// SubscriptionsRequester.h's `if (!Fr.Data) return;`. The target stayed dead until a reboot or a
// setup upload restored the block, which is why it looked like "only a reboot fixes it".
//
// Measured on the rig: the reachable source is a Register write whose ValueInfo says Size 0 (one
// such write took Dynamic.0 field 6 from meta 04000100 to 04000000, and a real 1-byte push on the
// subscription row afterwards renewed the row's lease while the field never changed). A status ack
// cannot bring one: Dispatcher.h:69-76 drops FLAG_SUCCESS/FAIL typed packets before the
// Subscriptions service sees them.
//
// The target here is the real DynamicBlockDescriptor - this header only needs MemoryTypes.h, so
// the storage half of the chain runs on the host. The requester function itself cannot be linked
// without the device layer (DeviceStatus, TxFrame, the packet codec), so the update arrives
// through the same predicate ApplyRequesterValue now returns on and the same SetEntry it writes
// through.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/MemoryBlocks.h"

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
            std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
                        #got, #want, g, w);                                          \
        }                                                                            \
    } while (0)

int main() {
    // ---- The decision itself: a value update with no value bytes is not a value.
    CHECK(!ValueUpdateApplicable(0));
    CHECK(ValueUpdateApplicable(1));
    CHECK(ValueUpdateApplicable(4));
    CHECK(ValueUpdateApplicable(255));

    // ---- A live Bool target: Dynamic.0 field 6, the field the bambi emote switch reads.
    DynamicBlockDescriptor Block;
    Block.Present = true;
    const ValueInfo Bool1 = { (uint16_t)DataType::Bool, 1, 0 };
    uint8_t One = 1;
    uint8_t Zero = 0;

    CHECK(Block.SetEntry(6, 0, &One, 1, Bool1));
    KeyResult K = Block.GetKey(6, 0);
    CHECK(K.Exists);
    CHECK_EQ(K.Meta.Size, 1);
    CHECK_EQ(K.DataLen, 1);
    CHECK(K.DataPtr != nullptr);
    if (K.DataPtr) CHECK_EQ(*(const uint8_t *)K.DataPtr, 1);

    // ---- A requester update arriving with length 0: refused, the target keeps its value.
    CHECK(!Block.SetEntry(6, 0, nullptr, 0, Bool1));
    K = Block.GetKey(6, 0);
    CHECK(K.Exists);
    CHECK_EQ(K.Meta.Size, 1);          // the descriptor did not collapse to Size 0
    CHECK(K.DataPtr != nullptr);       // so the field still resolves (no `!Fr.Data` drop)
    if (K.DataPtr) CHECK_EQ(*(const uint8_t *)K.DataPtr, 1); // previous value intact

    // ---- ... and the NEXT normal update still lands (the half that used to be lost for good).
    CHECK(Block.SetEntry(6, 0, &Zero, 1, Bool1));
    K = Block.GetKey(6, 0);
    CHECK(K.Exists);
    CHECK_EQ(K.Meta.Size, 1);
    CHECK(K.DataPtr != nullptr);
    if (K.DataPtr) CHECK_EQ(*(const uint8_t *)K.DataPtr, 0);

    // ---- A zero-length update cannot invent a key either.
    CHECK(!Block.SetEntry(7, 0, nullptr, 0, Bool1));
    CHECK(!Block.GetKey(7, 0).Exists);
    CHECK_EQ(Block.EntryCount, 1);

    // ---- Deleting still works (type None, no value bytes): the guard sits after the delete.
    const ValueInfo None = { (uint16_t)DataType::None, 0, 0 };
    CHECK(Block.SetEntry(6, 0, nullptr, 0, None));
    CHECK(!Block.GetKey(6, 0).Exists);
    CHECK_EQ(Block.EntryCount, 0);

    Block.Release();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
