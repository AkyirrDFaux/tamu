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
// E73 added the other half of the same decision: the question is asked at CONNECT as well, so a
// peer that could never be admitted is refused before its security procedure starts
// (`BleConnectRefused` at the bottom of this header).
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
// controller resolved a rotating private address (RPA) back to it. This IS the resolution witness on
// this build. When the connection-complete event carries an identity type (2/3), NimBLE's
// `ble_hs_conn_addrs()` keeps the event's identity address on `peer_id_addr` and sets `peer_ota_addr`
// to the RPA the peer actually used, so `ConnInfo.getIdAddress()` and `ConnInfo.getAddress()` differ
// exactly then. A peer that connects from its own public (or static random) address is reported as
// 0/1 and both fields carry the same address, so the pair reads equal - the normal case on a rig
// whose host cannot enable privacy, NOT evidence that the read-out is broken.
//
// The address TYPE is not a witness: `ble_hs_conn_addrs()` rewrites 2/3 to 0/1 on `peer_id_addr` in
// the same function, so the type the host is handed can never read 2/3. The `/N` text in the block's
// read-out is carried as information only.
inline bool BleCameInResolved(const uint8_t Air[6], uint8_t AirType, const uint8_t Id[6], uint8_t IdType)
{
    return !BleSameIdentity(Air, AirType, Id, IdType);
}

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

// The same question, asked where the link is ESTABLISHED rather than where it is admitted
// (Issues.md E73). WHERE it is asked decides what the peer sees, not only whether it gets in:
//
//  - asked only in `onAuthenticationComplete`, a phone that can never be admitted is still walked
//    through a pairing - the stack sends a Security Request at connect, the OS raises its pairing
//    dialog, the pairing completes without a bond (bonding is closed outside a window) and the link
//    is then dropped. The outcome is right and the presentation is wrong: the user is taught that
//    this device pairs, spends a pairing attempt, and sees a failure at the end of a dialog that
//    should never have opened.
//  - asked at CONNECT, the link is terminated in the connection-complete callback, before
//    `ble_gap_security_initiate()` and before any SMP exchange. The central's pairing UI has
//    nothing to react to, so the OS reports a connection failure rather than a pairing failure.
//
// Nothing is lost by refusing early, because the answer cannot be a false "no": a session needs the
// peer to encrypt with a stored LTK, `ble_sm_retrieve_ltk` looks that LTK up by the peer's IDENTITY
// address, so a link whose identity is not in the store has nothing to authenticate with - the
// security procedure could only have failed or produced a bond this policy refuses to keep.
//
// The same function, deliberately: the connect-time refusal and the session gate cannot disagree,
// and a peer that passes the first can never be refused by the second for a reason the first could
// have seen. What differs is only WHEN it is asked - a window that opens while a refused link is
// still tearing down does not retroactively admit it; the next connect is judged again.
inline bool BleConnectRefused(bool WindowOpen, int BondsBefore, bool PeerWasBonded)
{
    return !BleSessionAllowed(WindowOpen, BondsBefore, PeerWasBonded);
}
