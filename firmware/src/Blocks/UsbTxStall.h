#pragma once

// When the USB host has stopped draining the port.
//
// `usb_serial_jtag_is_connected()` is a SOF test: it stays true while a host holds the port open and
// reads nothing (an app that crashed, a host that opened /dev/ttyACM0 and went away, a suspended
// reader). So the port looks live, the TX pump keeps offering it bytes, and every one of them waits
// out its timeout in `usb_serial_jtag_write_bytes`. The driver's TX ring is 2 KB, so after a few
// replies the ring is full and nothing moves at all.
//
// A reply the port would not take is not a byte to wait for: the host that asked for it is not
// reading. This tracks the one thing that tells a host that is merely idle from a port that is not
// draining - a pass in which the port accepted NOTHING - and says when that has lasted long enough to
// be hopeless (USB_TX_STALL_MS). The pump then stops offering USB bytes until the host speaks again
// (a received frame is proof it is reading), which is what stops a dead port from costing a blocking
// pass over and over.
//
// Host-clean: test/native/usb_tx_stall_test.cpp drives it, AppUSB.h supplies "bytes moved" and the
// clock.

#include <cstdint>

// A port that takes nothing for this long is not a slow host: with the driver's 2 KB ring empty and
// a 20 ms cap on a pass, a reading host would have taken the reply inside one connection interval.
#define USB_TX_STALL_MS 1500

struct UsbTxStall
{
    bool     Active   = false; // a pass took nothing and the clock is running
    bool     Reported = false; // the current episode has already been reported (once)
    uint32_t SinceMs  = 0;
    uint32_t Episodes = 0;     // stalled episodes since boot (diagnostics)

    bool Stalled(uint32_t NowMs) const
    {
        return Active && (uint32_t)(NowMs - SinceMs) >= USB_TX_STALL_MS;
    }

    // The host showed life (a frame arrived): the port is being read again.
    void Clear()
    {
        Active   = false;
        Reported = false;
    }

    // One pass offered `Moved` bytes to the port and it took them (0 = it took nothing).
    void Note(uint16_t Moved, uint32_t NowMs)
    {
        if (Moved > 0)
        {
            Clear();
            return;
        }
        if (!Active)
        {
            Active   = true;
            SinceMs  = NowMs;
            Reported = false;
        }
    }

    // True exactly once per stalled episode - the pass that decides the port is dead - so the caller
    // purges the ring and logs it a single time instead of on every pass.
    bool TakeReport(uint32_t NowMs)
    {
        if (!Stalled(NowMs) || Reported)
            return false;
        Reported = true;
        Episodes = Episodes + 1;
        return true;
    }
};
