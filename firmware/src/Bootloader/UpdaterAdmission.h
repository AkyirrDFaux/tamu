#pragma once

// Who may write the main image to a pairing-free updater (Docs/Services/Bootloader.md: the
// bootloader is entered by holding a button; it has no security of its own).
//
// The updater has no display and - by design - no pairing, no bonds and no security manager,
// so there is no key it could hold that a peer in radio range could not read off the air. That
// rules out the obvious "passphrase in the protocol": a code typed into the phone travels in
// clear text on an unencrypted link, so it proves nothing to the device and everything to a
// listener. What is left, and what this header encodes, is physical presence:
//
//   - the radio only exists in update mode (a device running the app has no updater service
//     on the air at all), and update mode is entered by B1 at reset or by a failed-start
//     budget (BootDecision.h) - not by anything a link can ask for;
//   - inside update mode the advertisement lives for a bounded window and then goes off the
//     air, so an unattended device parked in update mode does not keep offering itself;
//   - a write is accepted only on a link whose window is still open, and only from the one
//     connection the updater holds (a second peer is disconnected).
//
// The blast radius of a stranger who does catch the window is bounded on purpose: the factory
// image itself is never written, an update only lands in `ota_0`, and an image that never
// proves healthy falls back to update mode. So the worst case is a device that needs another
// update - not a brick.

#include <cstdint>

namespace UpdaterAdmission
{
    // How long the updater advertises after update mode is entered. Long enough to find the
    // device with a phone and push a 700 KB image, short enough that "left in update mode"
    // does not mean "open to the room".
    constexpr uint32_t WINDOW_MS = 300000;

    // A window is only ever opened by entering update mode; it is measured from that moment.
    inline uint32_t WindowEnd(uint32_t NowMs) { return NowMs + WINDOW_MS; }

    // Signed compare, so a deadline further away than INT32_MAX reads as open rather than past.
    inline bool WindowOpen(uint32_t NowMs, uint32_t EndMs)
    {
        return EndMs != 0 && (int32_t)(EndMs - NowMs) > 0;
    }

    // A frame may only be accepted from the live connection while the window is open: the
    // connection does not outlive the window it was admitted by.
    inline bool WriteAllowed(bool WindowIsOpen, bool LinkUp)
    {
        return WindowIsOpen && LinkUp;
    }
}
