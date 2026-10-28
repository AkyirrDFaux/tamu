// Host-side test for the BLE admission rules (Blocks/BleAdmissionPolicy.h).
//
// AppBLE.h cannot be linked on the host (it pulls NimBLE, the host task and NVS), so the part of
// the rule that is a decision rather than a call into the stack lives in its own header and is
// driven here. Three things are checked, all without a radio:
//
//  1. Identity addresses compare across the two type spaces. The controller reports a peer whose
//     RPA it resolved as type 2/3 (public/random *identity*), while the bond store keeps the same
//     address as type 0/1 - a raw comparison never matches the owner of the device, and E66's
//     session gate is built on this comparison.
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

int main()
{
    TestSameIdentity();
    TestBondingAllowed();
    TestSessionAllowed();
    TestStrangerCannotBeAdmittedWhereBondingIsClosed();

    printf("ble admission: %d checks passed", checks - failures);
    if (failures != 0)
        printf(", %d FAILED", failures);
    printf("\n");
    return failures == 0 ? 0 : 1;
}
