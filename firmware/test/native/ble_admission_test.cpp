// Host-side test for the BLE admission rules (Blocks/BleAdmissionPolicy.h).
//
// AppBLE.h cannot be linked on the host (it pulls NimBLE, the host task and NVS), so the part of
// the rule that is a decision rather than a call into the stack lives in its own header and is
// driven here. Three things are checked, all without a radio:
//
//  1. Identity addresses compare across the two type spaces. The controller's connection-complete
//     event reports a peer whose RPA it resolved as type 2/3 (public/random *identity*), while the
//     bond store keeps the same address as type 0/1 - a raw comparison never matches the owner of
//     the device, and E66's session gate is built on this comparison. (NimBLE normalises 2/3 to 0/1
//     before the host sees the identity, so the tolerance is belt and braces rather than load-bearing
//     - but it is what keeps the gate correct if that ever changes.)
//
//  2. A new bond may only be made inside a pairing window, or while the store is still empty. This
//     is what keeps a stranger from making a bond and then reporting "bonded" on a later link.
//
//  3. Who may hold a session: the acceptance criterion of E66 - with a bond stored and no window,
//     the peer that owns the device gets a session and a peer that is not bonded does not. The
//     table below is the whole rule, including the property that makes it safe: wherever a new
//     bond cannot be made, a peer that was not bonded before cannot be admitted.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdio>
#include <cstring>

#include "Blocks/BleAdmissionPolicy.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

// BLE address types, spelled out so the test reads like the spec rather than like magic numbers.
enum
{
    Public = 0,
    Random = 1,
    PublicIdentity = 2,
    RandomIdentity = 3,
};

// 4C:23:38:74:E6:F6 - the rig's own hci0, i.e. the address the review read back as Bond[0].
static const uint8_t Phone[6] = {0xF6, 0xE6, 0x74, 0x38, 0x23, 0x4C};
// A different peer one byte off, for the negative cases.
static const uint8_t Other[6] = {0xF6, 0xE6, 0x74, 0x38, 0x23, 0x4D};

static void TestSameIdentity()
{
    // The pair that E66 depends on: the store keeps type 0, the controller reports a resolved peer
    // as type 2 with the same six bytes.
    CHECK(BleSameIdentity(Phone, Public, Phone, PublicIdentity));
    CHECK(BleSameIdentity(Phone, PublicIdentity, Phone, Public));
    // Same for a random (static) identity.
    CHECK(BleSameIdentity(Phone, Random, Phone, RandomIdentity));
    CHECK(BleSameIdentity(Phone, RandomIdentity, Phone, Random));

    // The public/random axis is part of the address, so it must not be normalised away.
    CHECK(!BleSameIdentity(Phone, Public, Phone, Random));
    CHECK(!BleSameIdentity(Phone, PublicIdentity, Phone, RandomIdentity));
    CHECK(!BleSameIdentity(Phone, Public, Phone, RandomIdentity));

    // ... and every byte counts, not just the first.
    CHECK(!BleSameIdentity(Phone, Public, Other, Public));
    CHECK(!BleSameIdentity(Phone, Public, Other, PublicIdentity));

    // Reflexive, and the type byte is taken as shipped (bit 0 is the public/random axis).
    CHECK(BleSameIdentity(Other, RandomIdentity, Other, RandomIdentity));
    CHECK(BleSameIdentity(Other, 0xFF, Other, 0xFF));
}

static void TestBondingAllowed()
{
    // A window is the one place a new phone may pair ...
    CHECK(BleBondingAllowed(true, 0));
    CHECK(BleBondingAllowed(true, 1));
    CHECK(BleBondingAllowed(true, 3));
    // ... plus the empty store, which is how the first bond is made with no USB host.
    CHECK(BleBondingAllowed(false, 0));
    // With a bond stored and no window, no new bond may be made - a stranger who pairs gets an
    // encrypted link without a bond, which the session gate refuses (and forgets).
    CHECK(!BleBondingAllowed(false, 1));
    CHECK(!BleBondingAllowed(false, 2));
    CHECK(!BleBondingAllowed(false, 3));
}

static void TestSessionAllowed()
{
    // E66's acceptance criterion, as a table: bond stored, no window.
    CHECK(BleSessionAllowed(false, 1, true));   // the owner reconnects - the case that failed
    CHECK(!BleSessionAllowed(false, 1, false)); // a peer that is not bonded gets no session
    CHECK(BleSessionAllowed(false, 3, true));   // ... with a full store
    CHECK(!BleSessionAllowed(false, 3, false));

    // The empty store admits a new peer: there is nothing to protect and this pairing is the
    // first bond (E64).
    CHECK(BleSessionAllowed(false, 0, false));
    CHECK(BleSessionAllowed(false, 0, true));

    // A window admits whoever the pairing policy admits, bonded or not.
    CHECK(BleSessionAllowed(true, 0, false));
    CHECK(BleSessionAllowed(true, 1, false));
    CHECK(BleSessionAllowed(true, 3, false));
    CHECK(BleSessionAllowed(true, 1, true));

    // A two-bond store still admits the peer that is one of them.
    CHECK(BleSessionAllowed(false, 2, true));
    CHECK(!BleSessionAllowed(false, 2, false));
}

// The property that makes rule 3 sufficient without a snapshot of "which phone": wherever a new
// bond cannot be made, a peer that was not bonded before cannot end up admitted. If this line ever
// breaks, a stranger can pair outside a window and be admitted on the same link.
static void TestStrangerCannotBeAdmittedWhereBondingIsClosed()
{
    for (int WindowOpen = 0; WindowOpen <= 1; WindowOpen++)
    {
        for (int Bonds = 0; Bonds <= 3; Bonds++)
        {
            if (BleBondingAllowed(WindowOpen, Bonds))
                continue;
            CHECK(!BleSessionAllowed(WindowOpen, Bonds, false));
            // ... while the peer that was already bonded is still admitted: closing the door must
            // never lock the owner out (E66).
            CHECK(BleSessionAllowed(WindowOpen, Bonds, true));
        }
    }
}

// The controller's vocabulary: types 2/3 are IDENTITY addresses, which is what the
// connection-complete event carries when the controller resolved a peer's RPA; 0/1 is the address as
// it came off the air (public / random, an unresolved RPA included). This test pins the ADDRESS-PAIR
// witness the E66 read-out uses (`ble_hs_conn_addrs`: peer_id_addr = the identity, peer_ota_addr = the
// RPA for a resolved peer), plus the two shape cases a rig will actually read: a peer that connects
// from its own address reads the pair EQUAL, and a resolved peer reads it DIFFERENT. The reported
// type is not a witness - NimBLE rewrites 2/3 to 0/1 before the host sees it.
static void TestCameInResolved()
{
    // A resolved peer: identity is the bonded public address, the address on the air is a random one.
    CHECK(BleCameInResolved(Other, Random, Phone, Public));
    // A peer that connected from its own address is NOT resolved - the same six bytes and type.
    CHECK(!BleCameInResolved(Phone, Public, Phone, Public));
    // The identity half can be either type space: `ble_hs_conn_addrs` normalises the event's 2/3 to
    // 0/1 on peer_id_addr, so a comparison that only worked on one space would be a landmine.
    CHECK(!BleCameInResolved(Phone, Random, Phone, Random));
    // Same bytes but a different public/random axis is still a different address.
    CHECK(BleCameInResolved(Phone, Random, Phone, Public));
}

// The verdict numbers are gone with the "Last link" field (the block carries six fields now, none of
// them a link record). What stays pinned is the two decisions the radio still asks this header for.

// The controller's own witness of a resolution: types 2/3 are IDENTITY addresses, which is exactly
// what it reports when it resolved a peer's RPA; 0/1 is the address as it came off the air
// (public / random, an unresolved RPA included). On this build the address PAIR is the witness -
// NimBLE sets peer_ota_addr to the peer's RPA for a resolved peer - so the reported type is only
// carried as information, and TestCameInResolved above is the check that matters.

int main()
{
    TestSameIdentity();
    TestCameInResolved();
    TestBondingAllowed();
    TestSessionAllowed();
    TestStrangerCannotBeAdmittedWhereBondingIsClosed();

    printf("ble admission: %d checks passed", checks - failures);
    if (failures != 0)
        printf(", %d FAILED", failures);
    printf("\n");
    return failures == 0 ? 0 : 1;
}
