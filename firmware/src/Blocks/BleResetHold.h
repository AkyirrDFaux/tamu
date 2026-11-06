#pragma once

// The USB-less BLE reset: how a HELD contact on GPIO8 is timed, and what the notification LED
// does while the sequence runs (owner, 2026-11-05, replacing the timings of that morning's brief).
//
// The owner's words: "Sub-10s restart stack + open window for 300 s, over 10s full clear. ignore
// first 2s. upon reseting and opening window light up red led until lifting the contact, after
// clearing blink it 5 times no matter if held, then keep dark and return the led."
//
// The reset input is GPIO8, an input with the internal pull-up: released = HIGH, contact closed = LOW.
//
//   held  < 2 s, released        -> nothing at all (the reset ignores the first 2 s)
//   held 2 s .. 10 s, released   -> restart the BLE stack, bonds KEPT, and open the pairing window
//                                   for BLE_AUTO_WINDOW_MS (300 s)
//   held >= 10 s                 -> full clear: the bonds are cleared and the stack restarted
//
// The 10 s action fires while the contact is still down, because it is "held >= 10 s" and not a
// release: there is nothing to wait for. The restart path can only fire ON the release - until the
// contact is lifted the firmware cannot know it is going to be lifted before 10 s.
//
// Two facts this header exists to get right, neither of which is obvious:
//
//   - **A boot-time LOW is not a hold.** GPIO8 is an ESP32-C3 strapping pin and has to be high at
//     reset, so a pin that reads low when the firmware starts is either a contact held across the
//     reset or a short - not a person holding the input of a running device. The hold timer only
//     starts after the pin has been seen RELEASED (HIGH) once, and a sequence that ends while the
//     contact is still down re-arms on the same rule (no second hold until a real release).
//   - **Bounce must not move a threshold.** A level is accepted only after it has been steady for
//     BLE_RESET_DEBOUNCE_MS; the hold itself is timed from the moment the RAW level changed, so a
//     10 s hold is exactly 10 s and not 10 s + debounce.
//
// The notification LED is GPIO2 - a pad of its own, shared with the button input, active low on both
// (Docs/Devices.md, "LED-Button ... button active low, LED active low"). GPIO8 and GPIO2 are separate
// pads: a contact on GPIO8 neither lights nor holds GPIO2, and the LED driver on GPIO2 cannot change
// what GPIO8 reads. So this sequencer DRIVES GPIO2 (output LOW = lit, INPUT with the pull-up = dark)
// while it owns the pin - from the 2 s mark of a hold, which is what lights the owner's "solid red
// until lifting the contact", and through the five blinks after a clear. It never drives GPIO2 HIGH:
// with the button held that is the pin shorting 3V3 to ground. The blinks' dark halves are INPUT
// with the pull-up, so the five gaps ARE visible while the GPIO8 contact is still down, and the
// sequence runs regardless of the contact, which is what the owner asked for.
//
// Because GPIO2 is shared, a line the sequencer drives low reads as a pressed button. The LED/Button
// block's report is therefore SUPPRESSED for exactly as long as the sequencer owns the pin
// (BleResetPinOwned: the Solid and Blink phases) and reads the pin normally again as soon as it is
// handed back - see ButtonUpdate in Devices/Tamu_v2.0A/Button.h. A contact on the GPIO8 reset pad is
// not a button press at all: the button is read from GPIO2 and from nowhere else.
//
// Host-clean on purpose (no GPIO, no ESP-IDF, no clock): test/native/ble_reset_test.cpp drives the
// whole timing table, the boot-level rule, the pin's ownership (which is what suppresses the button)
// and the blink schedule. The device glue (Devices/Tamu_v2.0A/BleResetPin.h) feeds it the pin level
// and the time, and performs the returned action through AppBLEStackReset().

#include <cstdint>

// A level must be steady this long before it is accepted as the pin's state: long enough for a
// mechanical contact's bounce, far too short to be noticed inside a 2 s window.
#define BLE_RESET_DEBOUNCE_MS 30u
// The first 2 s of any contact are ignored: a shorter contact does nothing at all.
#define BLE_RESET_IGNORE_MS 2000u
// Held this long: full clear (bonds cleared, then the stack restarted).
#define BLE_RESET_CLEAR_MS 10000u
// The five blinks after a clear: each half of a blink is 250 ms, so one blink is a 500 ms period
// (on, then off) and five of them take 2500 ms. The sequence starts ON, ends OFF, and then the pin
// goes back to the notification driver.
#define BLE_RESET_BLINK_HALF_MS 250u
#define BLE_RESET_BLINK_COUNT 5u
#define BLE_RESET_BLINK_TOTAL_MS (2u * BLE_RESET_BLINK_COUNT * BLE_RESET_BLINK_HALF_MS)

static_assert(BLE_RESET_DEBOUNCE_MS < BLE_RESET_IGNORE_MS,
              "the debounce must be far shorter than the ignore window, or a contact could not be timed");
static_assert(BLE_RESET_IGNORE_MS < BLE_RESET_CLEAR_MS,
              "the clear is the longer hold: a 10 s contact must pass through the 2 s window");
static_assert(BLE_RESET_BLINK_TOTAL_MS == 2500u,
              "five blinks of a 500 ms period are 2500 ms - the figure the card quotes");

// The two pairings of the LED, spelled out because they are easy to confuse: the notification LED is
// active LOW (the pin low lights it) and so is the button read (the pin low is a press).
#define BLE_RESET_LED_LIT_LEVEL 0
#define BLE_RESET_LED_DARK_LEVEL 1

// What one step of the pin asks the device to do.
enum class BleResetAction : uint8_t
{
    None = 0,                 // nothing crossed on this pass
    RestartStack = 1,         // 2..<10 s, released: restart the stack, keep the bonds, open the window
    ClearBondsAndRestart = 2, // >=10 s: clear the bonds, then restart the stack
};

// The action's name, for the log line the device prints. Pinned by the native test so the two paths
// cannot be reported as each other.
inline const char *BleResetActionName(BleResetAction Action)
{
    switch (Action)
    {
    case BleResetAction::RestartStack: return "released inside 10 s: restarting the BLE stack (bonds kept)";
    case BleResetAction::ClearBondsAndRestart: return "held 10 s: clearing the bonds, then restarting the BLE stack";
    case BleResetAction::None: break;
    }
    return "nothing";
}

// Who holds GPIO2 right now. Idle = the notification driver (OnLEDStateChange/ButtonUpdate); Solid =
// the reset hold is recognised and the sequencer owns the pin (it drives the LED, the button read is
// suppressed); Blink = the five blinks after a clear (the sequencer drives the pin).
enum class BleResetPhase : uint8_t
{
    Idle = 0,
    Solid = 1,
    Blink = 2,
};

inline bool BleResetPinOwned(const BleResetPhase Phase)
{
    return Phase == BleResetPhase::Solid || Phase == BleResetPhase::Blink;
}

// The pin's state machine. One instance, owned by the device glue; every field is touched by
// BleResetStep()/BleResetClearComplete() and by nothing else.
struct BleResetHold
{
    bool Initialised = false;      // the first sample has been taken (it IS the boot level)
    bool ReleasedSeen = false;     // the pin has read HIGH once since the last sequence: a hold may be timed
    bool AcceptedHigh = true;      // the debounced level (the pull-up idles HIGH)
    bool CandidateHigh = true;     // the raw level being debounced
    uint32_t CandidateSinceMs = 0; // TimeFromBoot() when the raw level last changed
    bool Holding = false;          // a contact is being timed
    uint32_t HoldStartMs = 0;      // when the RAW level went low for this contact
    bool Announced = false;        // the contact passed BLE_RESET_IGNORE_MS: it is a reset hold
    bool ClearFired = false;       // the 10 s action of this contact has been returned
    BleResetPhase Phase = BleResetPhase::Idle; // who owns GPIO2
    uint32_t PhaseSinceMs = 0;     // when the current blink half started
    uint8_t BlinkHalvesLeft = 0;   // blink halves still to run (10 = 5 blinks)
    bool LedLit = false;           // the level the sequencer wants on the pin (only meaningful while owned)
};

// What one pass of the pin produced.
struct BleResetOutput
{
    BleResetAction Action = BleResetAction::None; // the device performs this
    bool PinOwned = false;                        // the sequencer owns GPIO2 after this pass
    bool LedLit = false;                          // ... and it wants the LED lit (drive the pin low)
    bool HandBackPin = false;                     // ... and it just stopped owning it: restore the driver
};

// Feeds one sample of the reset input (true = HIGH/released) at `NowMs`, and returns what crossed.
//
// The thresholds are compared against timestamps, so a slow loop pass delays an action instead of
// losing it. Call it from one task only (the main loop).
inline BleResetOutput BleResetStep(BleResetHold &S, bool RawHigh, uint32_t NowMs)
{
    BleResetOutput Out;

    // 0. The blink sequence runs off the clock, not off the pin: "regardless of whether the contact
    //    is still held". Each half is BLE_RESET_BLINK_HALF_MS; the sequence ends after the fifth
    //    OFF half, which is when the pin goes back to the notification driver.
    if (S.Phase == BleResetPhase::Blink)
    {
        while (S.BlinkHalvesLeft != 0 && (uint32_t)(NowMs - S.PhaseSinceMs) >= BLE_RESET_BLINK_HALF_MS)
        {
            S.PhaseSinceMs += BLE_RESET_BLINK_HALF_MS;
            S.BlinkHalvesLeft--;
            if (S.BlinkHalvesLeft != 0)
                S.LedLit = !S.LedLit;
        }
        if (S.BlinkHalvesLeft == 0)
        {
            // Dark, and the driver gets the pin back. ReleasedSeen is NOT re-armed here: a contact
            // still closed must be released once more before it can time a new sequence.
            S.Phase = BleResetPhase::Idle;
            S.LedLit = false;
            S.Holding = false;
            S.HoldStartMs = 0;
            S.Announced = false;
            S.ClearFired = false;
            S.ReleasedSeen = false;
            Out.HandBackPin = true;
        }
        Out.PinOwned = BleResetPinOwned(S.Phase);
        Out.LedLit = S.LedLit;
        return Out;
    }

    // The first sample is the boot level, not a transition: it only says how the pin came up, and a
    // LOW here arms nothing (see the header note on the strapping pin).
    if (!S.Initialised)
    {
        S.Initialised = true;
        S.CandidateHigh = RawHigh;
        S.AcceptedHigh = RawHigh;
        S.CandidateSinceMs = NowMs;
        S.ReleasedSeen = RawHigh;
        return Out;
    }

    // 1. Debounce: the raw level is the candidate and becomes the accepted level only once it has
    //    been steady for the debounce period, so a contact's bounce never reaches the timer.
    if (RawHigh != S.CandidateHigh)
    {
        S.CandidateHigh = RawHigh;
        S.CandidateSinceMs = NowMs;
    }
    if (S.AcceptedHigh != S.CandidateHigh &&
        (uint32_t)(NowMs - S.CandidateSinceMs) >= BLE_RESET_DEBOUNCE_MS)
        S.AcceptedHigh = S.CandidateHigh;

    // 2. Released: the contact is over. What it was depends on how long it lasted, and only now can
    //    the sub-10 s path be decided - a contact released before the ignore window did nothing.
    if (S.AcceptedHigh)
    {
        if (S.Holding)
        {
            if (S.Announced && !S.ClearFired)
            {
                // 2 s .. <10 s: the restart. The sequencer has been driving the LED since the
                // window was passed, so handing the pin back is the end of "solid until lifting".
                // A contact that already fired the clear is not restarted a second time on its
                // release.
                Out.Action = BleResetAction::RestartStack;
                Out.HandBackPin = true;
            }
            // else: a contact released inside the ignore window. It did nothing for the reset, and
            // it is not a button press either - the button is read from GPIO2, this is GPIO8.
            S.Holding = false;
            S.HoldStartMs = 0;
            S.Announced = false;
            S.ClearFired = false;
            S.Phase = BleResetPhase::Idle;
            S.LedLit = false;
        }
        S.ReleasedSeen = true;
        Out.PinOwned = false;
        Out.LedLit = false;
        return Out;
    }

    // 3. A low level the firmware saw at boot (or right after a sequence that ended with the contact
    //    still down) is not a hold: nothing is timed until the pin has been released once.
    if (!S.ReleasedSeen)
        return Out;

    // 4. The hold. It is timed from the RAW edge (the debounce only delays the confirmation), so the
    //    thresholds are measured from the contact, not from 30 ms after it.
    if (!S.Holding)
    {
        S.Holding = true;
        S.HoldStartMs = S.CandidateSinceMs;
    }
    const uint32_t HeldMs = (uint32_t)(NowMs - S.HoldStartMs);

    // Both thresholds are judged on the RAW level: the 30 ms debounce tail after a lift still reads
    // "accepted low", and a contact of 1.99 s (or 9.99 s) has to stay nothing (or a restart), not be
    // promoted to the next action by the time the release confirmation takes.
    if (RawHigh)
    {
        Out.PinOwned = BleResetPinOwned(S.Phase);
        Out.LedLit = S.LedLit;
        return Out;
    }

    // 4a. The ignore window has passed: this is a reset hold. The sequencer takes the pin and drives
    //     the LED from here - which is also what makes the LED/Button block's report be suppressed
    //     (ButtonUpdate asks BleResetPinOwned).
    if (!S.Announced && HeldMs >= BLE_RESET_IGNORE_MS)
    {
        S.Announced = true;
        S.Phase = BleResetPhase::Solid;
        S.LedLit = true;
    }

    // 4b. The full clear. It fires while the contact is still down; the blinks start when the device
    //     reports the clear complete (BleResetClearComplete), so a slow bond delete does not eat them.
    if (S.Announced && !S.ClearFired && HeldMs >= BLE_RESET_CLEAR_MS)
    {
        S.ClearFired = true;
        Out.Action = BleResetAction::ClearBondsAndRestart;
    }

    Out.PinOwned = BleResetPinOwned(S.Phase);
    Out.LedLit = S.LedLit;
    return Out;
}

// The device calls this once the clear has finished (AppBLEStackReset(true) returned): the five
// blinks run from here, whatever the contact is doing.
inline void BleResetClearComplete(BleResetHold &S, uint32_t NowMs)
{
    if (!S.ClearFired)
        return;
    S.Phase = BleResetPhase::Blink;
    S.PhaseSinceMs = NowMs;
    S.BlinkHalvesLeft = (uint8_t)(2u * BLE_RESET_BLINK_COUNT);
    S.LedLit = true; // the first half is the LED ON
}
