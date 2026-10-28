#pragma once

// Whether the USB console stream is IN USE by the app link - i.e. whether a free-text device log may
// be written into it (Devices/Tamu_v2.0A/Log.h, Issues.md E67 route (d), owner ruling 2026-10-28
// "D is sufficient"). The rule lives here, on its own, for the same reason Blocks/BleWindowPolicy.h
// does: the callers cannot be linked on the host (Log.h pulls esp_log.h and the USJ glue,
// AppInterface.h pulls the packet layer and the RTOS shims), so the decision is what gets tested,
// and test/native/appconsole_test.cpp drives it through its whole truth table.
//
// The stream is in use when the host is up AND it is either
//   - TALKING: an app frame arrived inside the idle window (~500 ms, AppUsbSilent()), so a reply is
//     in flight or expected, or
//   - still OWED bytes: a frame sits in the TX ring and will go out in 60-byte chunks, and a line
//     written between two chunks splits it. That also covers an unsolicited push - a subscription
//     update replayed to the app needs no request to be in flight either.
// Cable presence alone is NOT "in use": usb_serial_jtag_is_connected() reads true ~0.4 s after a
// reset whenever a host merely HOLDS the port, which is why a rig session used to read the boot
// banner and nothing else. Silence is the whole point of this rule - it is what makes the device's
// own lines readable on the bench.
//
// The cost, stated rather than hidden: nothing is shown while an app is actually talking.
//
// Head/tail are the ring's free-running uint16 counters, so only equality means empty - after a wrap
// the head is numerically smaller than the tail and the ring still holds bytes.

#include <cstdint>

inline bool AppConsoleBusy(bool UsbActive, bool UsbSilent, uint16_t TxHead, uint16_t TxTail)
{
    return UsbActive && (!UsbSilent || TxHead != TxTail);
}
