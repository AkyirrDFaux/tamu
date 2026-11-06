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
//  2. that a write's move is ARMED, not applied: the live net and the core's address stay on the
//     old identity until the write's own reply has gone out (a reply built from the new address
//     cannot be matched by the client that addressed the old one - a write that landed reported as
//     a timeout, rig measurement t_cac38f19);
//  3. that applying the armed value moves the live net AND the core's address together, to
//     NetID.1 - in one step, never half-way;
//  4. that a refused write arms nothing and changes neither (a rejected 0x3F must not leave the
//     core on net 0x3F, which DeviceIsCore and the dispatcher both treat as "not this core's net");
//  5. that applying with nothing armed is a no-op (the reply path calls it after every static
//     write, not only after a net write);
//  6. the invariant the save paths rely on: after the write, the value they snapshot equals the
//     value the write stored, so a Save All cannot revert it.
//
// run.sh also greps the write site, because the trigger itself lives in a header the host cannot
// link (it pulls the board's static memory): the checks there are that the trigger arms the move
// and stores the value, that it does NOT move the live identity itself, and that the write handler
// applies the armed move only after it has sent the reply.

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

// The armed slot is process-wide (that is the point of it), so a test that leaves a value armed
// would leak into the next one. Draining it into throwaway variables is the reset.
static void Disarm() {
    uint8_t Net = 0;
    uint16_t Addr = 0;
    ApplyArmedNetId(Net, Addr);
}

static void TestStorable() {
    CHECK(!NetIdStorable(0));      // unassigned: the boot replaces it with a random one
    CHECK(NetIdStorable(1));       // the lowest usable net
    CHECK(NetIdStorable(39));      // this rig's net
    CHECK(NetIdStorable(0x3E));    // the highest usable net
    CHECK(!NetIdStorable(0x3F));   // all-nets broadcast: not a net a core can be on
    CHECK(!NetIdStorable(0x40));   // beyond the 6-bit field
    CHECK(!NetIdStorable(0xFF));
}

// The fix itself: the write only arms the move. Both halves of the identity - the net every
// packet's id_src and target resolution are built from, and the address itself - stay on the old
// value while the acknowledgement of that very write is on its way out.
static void TestArmDoesNotMoveLive() {
    Disarm();
    LiveStatus S = Seed(14);
    const LiveStatus Before = S;
    CHECK(ArmNetId(39));
    CHECK(S.NetId == Before.NetId);                     // still 14
    CHECK(S.ShortAddress == Before.ShortAddress);       // still 14.1, so 14->39's reply leaves from it
    Disarm();
    CHECK(S.NetId == Before.NetId);                     // draining the slot moves nothing by itself
    CHECK(S.ShortAddress == Before.ShortAddress);
}

static void TestApplyMovesBoth() {
    Disarm();
    LiveStatus S = Seed(14);
    CHECK(ArmNetId(39));
    ApplyArmedNetId(S.NetId, S.ShortAddress);
    CHECK(S.NetId == 39);
    CHECK(S.ShortAddress == MakeId(39, 1));
    CHECK(((S.ShortAddress >> 10) & 0x3F) == 39);
    CHECK((S.ShortAddress & 0x3FF) == 1); // a core is NetID.1

    // And back again: the rule is symmetric, not a one-way migration.
    CHECK(ArmNetId(14));
    ApplyArmedNetId(S.NetId, S.ShortAddress);
    CHECK(S.NetId == 14);
    CHECK(S.ShortAddress == MakeId(14, 1));
}

// No window where the net and the address disagree, for every net there is - and one apply is
// enough: the slot is cleared, so a second call cannot move the core again.
static void TestEveryNet() {
    for (unsigned Net = 1; Net < 0x3F; Net++) {
        Disarm();
        LiveStatus S = Seed(1);
        CHECK(ArmNetId((uint8_t)Net));
        ApplyArmedNetId(S.NetId, S.ShortAddress);
        CHECK(S.NetId == (uint8_t)Net);
        CHECK(S.ShortAddress == MakeId((uint8_t)Net, 1));

        ApplyArmedNetId(S.NetId, S.ShortAddress); // nothing armed: must not move anything
        CHECK(S.NetId == (uint8_t)Net);
        CHECK(S.ShortAddress == MakeId((uint8_t)Net, 1));
    }
}

static void TestRefusedLeavesStatusAlone() {
    Disarm();
    const LiveStatus Before = Seed(14);
    LiveStatus S = Before;
    CHECK(!ArmNetId(0));
    CHECK(!ArmNetId(0x3F));
    ApplyArmedNetId(S.NetId, S.ShortAddress); // nothing was armed by the refusals
    CHECK(S.NetId == Before.NetId);
    CHECK(S.ShortAddress == Before.ShortAddress);
}

// A refused write must not leave a previously armed value behind either (the reply path applies
// whatever is armed after every static write).
static void TestRefusedClearsNothingItDidNotArm() {
    Disarm();
    LiveStatus S = Seed(14);
    CHECK(ArmNetId(39));   // an earlier accepted write
    Disarm();              // ... its reply path ran
    CHECK(!ArmNetId(0x3F));
    ApplyArmedNetId(S.NetId, S.ShortAddress);
    CHECK(S.NetId == 14);
    CHECK(S.ShortAddress == MakeId(14, 1));
}

// The invariant the save paths need: they mirror the LIVE net into `.SV`, so the write has to have
// moved the live net - otherwise a Save All puts the old one back (what a restore hit).
static void TestSaveCannotRevert() {
    Disarm();
    LiveStatus S = Seed(14);
    const uint8_t Stored = 39; // what the write puts in the static space
    CHECK(ArmNetId(Stored));
    ApplyArmedNetId(S.NetId, S.ShortAddress); // the write path does this once the reply is out
    const uint8_t Mirrored = S.NetId; // RegisterPersist.h / BleAutoName.h / BleSettingsGlue.h
    CHECK(Mirrored == Stored);
}

int main() {
    TestStorable();
    TestArmDoesNotMoveLive();
    TestApplyMovesBoth();
    TestEveryNet();
    TestRefusedLeavesStatusAlone();
    TestRefusedClearsNothingItDidNotArm();
    TestSaveCannotRevert();

    if (failures == 0) {
        std::printf("PASS: net-ID rule (%d checks)\n", checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks\n", failures, checks);
    return 1;
}
