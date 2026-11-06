// Host-side test for the core's net-ID rule (Core/Functions/NetId.h).
//
// The rule exists because the two halves of the net-ID - the live status every packet's id_src is
// built from, and the value the save paths mirror into `.SV` - drifted apart: the register write
// stored the space alone, so the next Save All (which snapshots the live net) reverted it, and a
// restore therefore could not move a wiped core onto the setup's net. With the core on a net no
// node answers, every subscription install (CID 0x01 Change) is addressed into nothing.
//
// So this pins:
//  1. which values are nets at all (0 = unassigned, 0x3F = all nets: both refused);
//  2. that a valid write moves the live net AND the core's address together, to NetID.1;
//  3. that a refused write changes neither (a rejected 0x3F must not leave the core on net 0x3F,
//     which DeviceIsCore and the dispatcher both treat as "not this core's net");
//  4. the invariant the save paths rely on: after the write, the value they snapshot equals the
//     value the write stored, so a Save All cannot undo it.
//
// run.sh also greps the write site, because the trigger itself lives in a header the host cannot
// link (it pulls the board's static memory): the check there is that the trigger goes through
// ApplyNetId instead of storing the space alone.

#include <cstdint>
#include <cstdio>

#include "Core/Functions/NetId.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

// What a core's status looks like: the live net and the bus address built from it.
struct LiveStatus {
    uint8_t NetId;
    uint16_t ShortAddress;
};

static LiveStatus Seed(uint8_t NetId) { return LiveStatus{NetId, MakeId(NetId, 1)}; }

static void TestStorable() {
    CHECK(!NetIdStorable(0));      // unassigned: the boot replaces it with a random one
    CHECK(NetIdStorable(1));       // the lowest usable net
    CHECK(NetIdStorable(39));      // this rig's net
    CHECK(NetIdStorable(0x3E));    // the highest usable net
    CHECK(!NetIdStorable(0x3F));   // all-nets broadcast: not a net a core can be on
    CHECK(!NetIdStorable(0x40));   // beyond the 6-bit field
    CHECK(!NetIdStorable(0xFF));
}

static void TestApplyMovesBoth() {
    LiveStatus S = Seed(14);
    CHECK(ApplyNetId(S.NetId, S.ShortAddress, 39));
    CHECK(S.NetId == 39);
    CHECK(S.ShortAddress == MakeId(39, 1));
    CHECK(((S.ShortAddress >> 10) & 0x3F) == 39);
    CHECK((S.ShortAddress & 0x3FF) == 1); // a core is NetID.1

    // And back again: the rule is symmetric, not a one-way migration.
    CHECK(ApplyNetId(S.NetId, S.ShortAddress, 14));
    CHECK(S.NetId == 14);
    CHECK(S.ShortAddress == MakeId(14, 1));
}

// No window where the net and the address disagree, for every net there is.
static void TestEveryNet() {
    for (unsigned Net = 1; Net < 0x3F; Net++) {
        LiveStatus S = Seed(1);
        CHECK(ApplyNetId(S.NetId, S.ShortAddress, (uint8_t)Net));
        CHECK(S.NetId == (uint8_t)Net);
        CHECK(S.ShortAddress == MakeId((uint8_t)Net, 1));
    }
}

static void TestRefusedLeavesStatusAlone() {
    const LiveStatus Before = Seed(14);
    LiveStatus S = Before;
    CHECK(!ApplyNetId(S.NetId, S.ShortAddress, 0));
    CHECK(S.NetId == Before.NetId);
    CHECK(S.ShortAddress == Before.ShortAddress);

    CHECK(!ApplyNetId(S.NetId, S.ShortAddress, 0x3F));
    CHECK(S.NetId == Before.NetId);
    CHECK(S.ShortAddress == Before.ShortAddress);
}

// The invariant the save paths need: they mirror the LIVE net into `.SV`, so the write has to have
// moved the live net - otherwise a Save All puts the old one back (what a restore hit).
static void TestSaveCannotRevert() {
    LiveStatus S = Seed(14);
    const uint8_t Stored = 39; // what the write puts in the static space
    CHECK(ApplyNetId(S.NetId, S.ShortAddress, Stored));
    const uint8_t Mirrored = S.NetId; // RegisterPersist.h / BleAutoName.h / BleSettingsGlue.h
    CHECK(Mirrored == Stored);
}

int main() {
    TestStorable();
    TestApplyMovesBoth();
    TestEveryNet();
    TestRefusedLeavesStatusAlone();
    TestSaveCannotRevert();

    if (failures == 0) {
        std::printf("PASS: net-ID rule (%d checks)\n", checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks\n", failures, checks);
    return 1;
}
