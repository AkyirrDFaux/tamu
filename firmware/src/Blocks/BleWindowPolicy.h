#pragma once

// When the automatic (unasked) pairing window may open - Issues.md E69.
//
// The window is the PAIRING state: while one is open, any phone may connect and pair, and
// `BleSessionAllowed` admits whatever pairs inside it (Blocks/BleAdmissionPolicy.h). So who may
// cause one is a security question, and E69 is the answer it used to get wrong: the device opened
// it whenever the bond store merely READ empty. A peer that presents a bonded identity and asks to
// pair again makes the vendored stack delete that bond before any application callback runs
// (`BLE_GAP_EVENT_REPEAT_PAIRING` -> `ble_store_util_delete_peer` + `RETRY`,
// managed_components/h2zero__esp-nimble-cpp/src/NimBLEServer.cpp:607-625), so a link-driven
// deletion could empty the store and make a locked device offer itself to the room for five
// minutes. Reading the store is not an event and cannot be one: the device has no way to tell a
// deletion it did from one a link did, and it does not need to - a window is only ever owed to a
// thing the OWNER did.
//
// There are exactly three of those:
//   - the radio came up with no bond stored (headless first use, including a device that boots with
//     an empty store): without this nobody could ever make the first bond;
//   - an explicit forget emptied the store - zeroing the `Bonds` array, a passkey change that invalidates the
//     stored bonds, or the epoch revoke at radio start (all three run through
//     AppBLEForgetAllBonds, AppBLE.h), because every phone that was paired has to pair again;
//   - a hardware reset of the BLE stack: the GPIO8 input held down (Blocks/BleResetHold.h). The
//     owner is standing at the device, and the whole point of that input is to re-pair without a
//     USB host - so this event opens the window whether or not a bond is kept.
//
// The bytes the rule needs are two: which event happened, and what the store holds now. Both are
// host-clean, so test/native/ble_window_test.cpp drives the table and the radio code only supplies
// the two inputs.
//
// Nothing here is about the MANUAL window (a write to the block's `Window` field): that one is opened
// by the owner's command and does not consult this header.

#include <cstdint>

// What put the automatic window in the queue. `None` is the resting state and the state the device
// is in on every loop pass nothing happened in - importantly also the state after a link deleted a
// bond, which is why that deletion cannot open a window.
// The numbers are internal (no wire, no block field): they are pinned by the native test only so a
// renumbering is a deliberate edit.
enum BleWindowCause : uint8_t
{
    BleWindowCauseNone = 0,        // no event: the store may read empty, that is not a reason
    BleWindowCauseRadioStart = 1,  // the radio came up with an empty store (headless first use)
    BleWindowCauseForgotten = 2,   // an explicit forget emptied the store
    BleWindowCauseReset = 3,       // the GPIO8 BLE reset was performed (bonds kept or cleared)
};

// The cause's name, for the log line that says why the window opened. Pinned by the native test.
inline const char *BleWindowCauseName(BleWindowCause Cause)
{
    switch (Cause)
    {
    case BleWindowCauseRadioStart: return "the radio came up with no bond";
    case BleWindowCauseForgotten: return "the bonds were forgotten";
    case BleWindowCauseReset: return "the GPIO8 reset";
    case BleWindowCauseNone: break;
    }
    return "nothing";
}

// Does this event owe the automatic window? `BondsInStore` is what the store holds NOW: a store
// that already has a bond has nothing to recover, and a recovery window over it would be a pairing
// state nobody asked for - EXCEPT after a hardware reset, where the owner is at the device and
// re-pairing without USB is the point of the input. That one cause therefore grants either way;
// it is a physical presence, not a reading of the store, so it cannot be caused by a link.
inline bool BleAutoWindowGranted(BleWindowCause Cause, int BondsInStore)
{
    if (Cause == BleWindowCauseReset)
        return true;
    if (BondsInStore != 0)
        return false;
    return Cause == BleWindowCauseRadioStart || Cause == BleWindowCauseForgotten;
}
