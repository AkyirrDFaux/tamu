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

    // ---- A dictionary marker CREATE (t_2da784f0): a render part's kind lives only on its field's
    // key-0 row, which carries the dictionary's type and no value. Before this, no host write could
    // create one: every empty write was refused, so a setup restore could append a field to a live
    // block but the renderer could not classify it (kind 0, never applied).
    DynamicBlockDescriptor Parts;
    Parts.Present = true;
    const ValueInfo Geo0 = { (uint16_t)DataType::Geometry, 0, 0 };
    const ValueInfo Tex0 = { (uint16_t)DataType::Texture, 0, 0 };
    const ValueInfo Keyed0 = { (uint16_t)DataType::UnknownKeyed, 0, 0 };

    CHECK(ValueIsDictionaryType((uint16_t)DataType::Geometry));
    CHECK(ValueIsDictionaryType((uint16_t)DataType::Texture));
    CHECK(ValueIsDictionaryType((uint16_t)DataType::UnknownKeyed));
    CHECK(!ValueIsDictionaryType((uint16_t)DataType::Number));
    CHECK(!ValueIsDictionaryType((uint16_t)DataType::Bool));
    CHECK(!ValueIsDictionaryType((uint16_t)DataType::None));

    // An empty Geometry write at (9, 0) creates the marker row.
    CHECK(Parts.SetEntry(9, 0, nullptr, 0, Geo0));
    KeyResult M = Parts.GetKey(9, 0);
    CHECK(M.Exists);
    CHECK_EQ(ValueInfoType(M.Meta.Type), (uint16_t)DataType::Geometry);
    CHECK_EQ(M.Meta.Size, 0);     // the row carries no value of its own
    CHECK_EQ(M.DataLen, 0);
    CHECK(M.DataPtr == nullptr);  // ... and resolves to no data, which is what a marker is
    CHECK_EQ(Parts.EntryCount, 1);

    // The field is now a field: a value key appended behind it lands normally.
    const ValueInfo Shape = { (uint16_t)DataType::Enum, 1, 0 };
    uint8_t Fill = 1;
    CHECK(Parts.SetEntry(9, 1, &Fill, 1, Shape));
    KeyResult S = Parts.GetKey(9, 1);
    CHECK(S.Exists);
    CHECK_EQ(S.Meta.Size, 1);
    CHECK(S.DataPtr != nullptr);
    if (S.DataPtr) CHECK_EQ(*(const uint8_t *)S.DataPtr, 1);
    // ... and the marker row still reads back as a marker.
    M = Parts.GetKey(9, 0);
    CHECK(M.Exists);
    CHECK_EQ(ValueInfoType(M.Meta.Type), (uint16_t)DataType::Geometry);
    CHECK_EQ(M.Meta.Size, 0);
    CHECK(M.DataPtr == nullptr);
    CHECK_EQ(Parts.EntryCount, 2);

    // The same write again is idempotent (a restore may re-send it).
    CHECK(Parts.SetEntry(9, 0, nullptr, 0, Geo0));
    CHECK_EQ(Parts.EntryCount, 2);
    CHECK_EQ(Parts.GetKey(9, 0).Meta.Size, 0);

    // Texture and UnknownKeyed markers work the same way; the field table still lists them.
    CHECK(Parts.SetEntry(10, 0, nullptr, 0, Tex0));
    CHECK_EQ(ValueInfoType(Parts.GetKey(10, 0).Meta.Type), (uint16_t)DataType::Texture);
    CHECK(Parts.SetEntry(11, 0, nullptr, 0, Keyed0));
    CHECK_EQ(ValueInfoType(Parts.GetKey(11, 0).Meta.Type), (uint16_t)DataType::UnknownKeyed);
    {
        uint8_t Fields[16];
        CHECK_EQ(Parts.ListFields(Fields, 16), 3);
        CHECK_EQ(Fields[0], 9);
        CHECK_EQ(Fields[1], 10);
        CHECK_EQ(Fields[2], 11);
    }

    // ---- ... while an EMPTY write of any other type is still refused and leaves no row: the
    // poisoning case the gate exists for (a size-0 value row is dead for every later write).
    const ValueInfo Num1 = { (uint16_t)DataType::Number, 4, 0 };
    CHECK(!Parts.SetEntry(12, 3, nullptr, 0, Num1));
    CHECK(!Parts.GetKey(12, 3).Exists);
    CHECK(!Parts.SetEntry(9, 4, nullptr, 0, Num1)); // empty Number behind an existing field
    CHECK(!Parts.GetKey(9, 4).Exists);

    // ---- A row that already holds bytes is never emptied by an empty write, marker type or not:
    // this is the measured bambi shape, and the marker create must not reopen it.
    CHECK(Parts.SetEntry(9, 1, &Fill, 1, Shape));
    CHECK(!Parts.SetEntry(9, 1, nullptr, 0, Shape));
    S = Parts.GetKey(9, 1);
    CHECK(S.Exists);
    CHECK_EQ(S.Meta.Size, 1);
    CHECK(S.DataPtr != nullptr);
    if (S.DataPtr) CHECK_EQ(*(const uint8_t *)S.DataPtr, 1);
    // A Geometry row that somehow carries bytes is a value row too: an empty Geometry write to it
    // is refused, not converted into a marker.
    DynamicBlockDescriptor Odd;
    Odd.Present = true;
    const ValueInfo Geo4 = { (uint16_t)DataType::Geometry, 4, 0 };
    uint32_t Four = 0x04030201;
    CHECK(Odd.SetEntry(9, 0, &Four, 4, Geo4));
    CHECK(!Odd.SetEntry(9, 0, nullptr, 0, Geo0));
    CHECK_EQ(Odd.GetKey(9, 0).Meta.Size, 4);
    CHECK(Odd.GetKey(9, 0).DataPtr != nullptr);
    Odd.Release();

    Parts.Release();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
