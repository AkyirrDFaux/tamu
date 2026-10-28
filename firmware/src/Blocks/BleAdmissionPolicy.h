#pragma once

// Who may hold a BLE session on this device - Issues.md E64 for the rule, E66 for why the
// controller cannot enforce it.
//
// E64 split "who may connect" in two: advertising is ONE condition (no live connection), and the
// admission of a peer is a separate rule. That rule was the controller's accept list -
// whitelist-only while a bond is stored and no pairing window is open. E66 showed it cannot work
// on this controller: the accept list is matched against the address the peer uses ON THE AIR, and
// a phone under LE privacy connects from a rotating private address (RPA). The controller does
// hold the peer's IRK and does resolve that RPA - that is how the stored bond authenticates when
// the accept list is not in force - but it does not apply the resolution to the accept-list
// filter, so the filter drops the very peer it exists to admit.
//
// What is left is a decision the host can make. A link becomes a SESSION only when it proves a
// bond; the stack reports that in `onAuthenticationComplete` (encrypted && bonded) and the peer
// side of the bond cannot be spoofed by a stranger, because a NEW bond can only be made inside a
// pairing window (`BleBondingAllowed`). Outside a window a pairing produces no bond, so the stack
// reports enc=1 bonded=0 and the link is dropped - and the bond the stranger just failed to make
// is forgotten in the same callback, so nothing of it is left behind.
//
// Both rules are kept on purpose, though the second cannot fire while the first holds: if the
// bonding switch is ever lost (a stack change, a stale store, a peer that pairs before the flag is
// set) the session gate still refuses the link instead of admitting a stranger.
//
// Host-clean on purpose (no NimBLE, no ESP-IDF): test/native/ble_admission_test.cpp drives the
// same decisions the radio code makes.

#include <cstdint>

// BLE address types, restated so this header stays testable on the host:
// 0 public, 1 random (static or private), 2 public identity, 3 random (static) identity.
constexpr uint8_t BleAddrRandom = 1;

// The two halves of an identity address - the six bytes and whether they are public or random -
// are the same. A peer whose RPA the controller resolved arrives as an *identity*: the HCI
// connection-complete event carries type 2 (public identity) or 3 (random identity), and NimBLE's
// `ble_hs_conn_addrs()` normalises that to 0/1 on `peer_id_addr` while keeping the address the peer
// actually used in `peer_ota_addr`. The bond store keeps the same address as 0/1, so this comparison
// has to be right in BOTH type spaces: a raw type comparison fails whenever the two sides disagree
// on the byte, and that would lock the owner of the device out.
inline bool BleSameIdentity(const uint8_t A[6], uint8_t AType, const uint8_t B[6], uint8_t BType)
{
    if ((AType & 0x01) != (BType & 0x01))
        return false;
    for (uint8_t i = 0; i < 6; i++)
    {
        if (A[i] != B[i])
            return false;
    }
    return true;
}

// The peer connected from an address that is NOT the identity the admission decision used - i.e. the
// controller resolved a rotating private address (RPA) back to it, which is the E66 mechanism seen
// from the host: `ble_hs_conn_addrs()` keeps the event's identity address as `peer_id_addr` and its
// peer-RPA field as `peer_ota_addr`, so the two differ exactly when a resolution happened. This is
// the one fact that tells "the owner came back from a new RPA" apart from "an address arrived and the
// bond happened to match".
inline bool BleCameInResolved(const uint8_t Air[6], uint8_t AirType, const uint8_t Id[6], uint8_t IdType)
{
    return !BleSameIdentity(Air, AirType, Id, IdType);
}

// How the admission of one link ended, published as the `BLE Settings` block's "Last link" field
// (Blocks/BleSettings.h, field 11) so the verdict can be READ on a rig. The log cannot carry it:
// DeviceLog is dropped while a host holds the link (Issues.md E67), which is exactly when a connect
// happens. E66's open gap was that "a stranger was refused" could not be told apart from "the bonded
// peer's address was not recognised" - these values answer that question separately.
// The numbers are wire values (the app decodes them), so they are pinned by
// test/native/ble_admission_test.cpp and must not be reordered.
enum BleLinkVerdict : uint8_t
{
    BleLinkNone = 0,                // no link judged since boot
    BleLinkPending = 1,             // connected and not judged yet (the security deadline is what ends these)
    BleLinkAdmitted = 2,            // a session: encrypted, bonded, and one of the bonds from before the link
    BleLinkRefusedNotBonded = 3,    // this identity is not a bond and no window is open: a stranger, at connect
    BleLinkRefusedOutsideWindow = 4,// it paired on a link that may not make a bond; that bond was forgotten
    BleLinkRefusedUnsecured = 5,    // the link failed the security policy (no bond, or a passkey not used)
    BleLinkRefusedBusy = 6,         // a session was already live, or the block switched the radio off
};

// A new bond may only be made inside a pairing window, or while the store is still empty (the
// first bond, which is how a headless device is set up without a USB host). This is the pairing
// half of E64, and it is what makes the session gate below sufficient on its own: a peer that
// cannot make a bond cannot report one.
inline bool BleBondingAllowed(bool WindowOpen, int BondsInStore)
{
    return WindowOpen || BondsInStore == 0;
}

// Who may hold a session, asked when the security procedure completes. `PeerWasBonded` is "the
// peer's identity address was in the bond store when this link came up" - the snapshot is taken at
// connect, before the link can change the store.
//
//  - a window is open: a new phone may pair, and whoever is already bonded may reconnect;
//  - nothing is stored yet: this pairing is the first bond;
//  - otherwise only a peer we already had a key for - which, on an LE-privacy link, is the owner
//    reconnecting from an address the controller resolved back to their identity.
inline bool BleSessionAllowed(bool WindowOpen, int BondsBefore, bool PeerWasBonded)
{
    if (WindowOpen)
        return true;
    if (BondsBefore == 0)
        return true;
    return PeerWasBonded;
}
