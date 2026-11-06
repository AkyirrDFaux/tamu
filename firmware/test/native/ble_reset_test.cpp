// Host-side test for the GPIO8 BLE reset's timing table, its LED sequence and the button whose pin it
// shares (Blocks/BleResetHold.h).
//
// The owner's specification (2026-11-05): the first 2 s of a contact are ignored; a contact held 2 s
// to 10 s and released restarts the BLE stack with the bonds kept and opens the 5-minute pairing
// window; a contact held 10 s or longer is a full clear. The notification LED (GPIO2, active low,
// shared with the button input) is lit by the sequencer from the recognition of the hold until the
// contact is lifted, and after a clear it blinks five times (500 ms period, 2500 ms total) regardless
// of the contact, then goes dark and the pin returns to the notification driver. The button's own
// report is suppressed for as long as the sequencer owns that pin - a line it drives low reads as a
// press - and works again afterwards.
//
// What is measured here, in order:
//   1. the timing table: 1.9 s -> nothing, 2 / 5 / 9.9 s -> one restart, 10 s -> one clear;
//   2. the LED: owned and lit from the 2 s mark, handed back at the restart's release, and the exact
//      blink schedule after a clear (five lit runs of 250 ms, 2500 ms in total);
//   3. a boot-time LOW is not a hold (the strapping-pin rule), and a sequence that ends with the
//      contact still down re-arms only on a real release;
//   4. the button (ButtonState, the LED/Button block's field 0): never reported while the sequencer
//      owns GPIO2, a press of the real button reported again once the pin is handed back, and a
//      contact on the GPIO8 reset pad never a button press at all.

#include <cstdio>

#include "Blocks/BleResetHold.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

// The loop pass: 10 ms of wall clock per tick, which is the shape of the device's main loop (2 ms
// sleep, several services per pass).
static const uint32_t TickMs = 10;

// The LED/Button block's field 0, exactly as Devices/Tamu_v2.0A/Button.h composes it: the button is
// the GPIO2 line (active low, so a low line is a press), and the report is forced false while the
// notification driver holds the line (LED on) or the reset sequencer owns it. The run.sh source guard
// pins Button.h to those three terms, so this model and the device code cannot drift apart silently.
//   Gpio2High  = PinRead(LED_NOTIFICATION_PIN)   (true = released)
//   LedOn      = StaticVol.LedButton.LEDState
//   ResetOwns  = BleResetOwnsPin()
static bool ButtonStateFrom(bool Gpio2High, bool LedOn, bool ResetOwns)
{
    if (LedOn || ResetOwns)
        return false;
    return !Gpio2High;
}

struct Rig
{
    BleResetHold S;
    uint32_t Now = 0;
    int Restarts = 0, Clears = 0, HandBacks = 0;
    uint32_t RestartAt = 0, ClearAt = 0;
    bool PinOwnedEver = false, LedLitEver = false;
    int BlinkLitRuns = 0;        // distinct lit phases (one per blink)
    uint32_t BlinkLitMs = 0;     // total lit time, one blink = 250 ms
    bool PrevLit = false;
    bool PrevBlink = false;

    void Tick(bool High)
    {
        Now += TickMs;
        const BleResetOutput Out = BleResetStep(S, High, Now);
        if (Out.Action == BleResetAction::RestartStack)
        {
            Restarts++;
            RestartAt = Now;
        }
        if (Out.Action == BleResetAction::ClearBondsAndRestart)
        {
            Clears++;
            ClearAt = Now;
            // The device reports the clear complete in the same pass it performed the clear.
            BleResetClearComplete(S, Now);
        }
        Out.HandBackPin ? HandBacks++ : 0;
        PinOwnedEver = PinOwnedEver || Out.PinOwned;
        LedLitEver = LedLitEver || Out.LedLit;
        if (S.Phase == BleResetPhase::Blink)
        {
            // The blinks are the only time the sequencer drives the pin; count the lit phases there.
            // The sequence's first half is ON, so entering the phase starts a run.
            if (Out.LedLit && (!PrevLit || !PrevBlink))
                BlinkLitRuns++;
            if (Out.LedLit)
                BlinkLitMs += TickMs;
        }
        PrevBlink = S.Phase == BleResetPhase::Blink;
        PrevLit = Out.LedLit;
    }

    // Runs from the current time to `UntilMs` with the pin HIGH (released) or LOW (contact closed).
    void RunUntil(uint32_t UntilMs, bool High)
    {
        while (Now + TickMs <= UntilMs)
            Tick(High);
    }
};

// A press at `PressMs` held `HoldMs`: the contact closes at PressMs and lifts at PressMs + HoldMs.
// Everything before PressMs and after the lift is released. Returns the rig for inspection.
static Rig RunContact(uint32_t PressMs, uint32_t HoldMs, uint32_t AfterMs = 500)
{
    Rig R;
    R.Tick(true); // the boot level: released
    R.RunUntil(PressMs - TickMs, true);
    R.RunUntil(PressMs + HoldMs, false); // the first LOW pass is at PressMs
    R.RunUntil(PressMs + HoldMs + AfterMs, true);
    return R;
}

static void TestTimingTable()
{
    // 1.9 s: the first 2 s are ignored. Nothing happens at all - no restart, no clear, no LED, no
    // hand-back (the pin was never owned).
    {
        Rig R = RunContact(1000, 1900);
        CHECK(R.Restarts == 0);
        CHECK(R.Clears == 0);
        CHECK(R.HandBacks == 0);
        CHECK(!R.PinOwnedEver);
        CHECK(!R.LedLitEver);
        printf("  1.9 s hold  -> %d restart(s), %d clear(s), LED owned: %s\n", R.Restarts, R.Clears,
               R.PinOwnedEver ? "yes" : "no");
    }

    // 2 s .. 9.9 s: exactly one restart, never a clear. The bounds are the table's, so all three
    // points are driven, including both ends.
    {
        const uint32_t Holds[] = {2000, 5000, 9900};
        for (uint32_t Hold : Holds)
        {
            Rig R = RunContact(1000, Hold);
            CHECK(R.Restarts == 1);
            CHECK(R.Clears == 0);
            CHECK(R.HandBacks == 1);
            CHECK(R.PinOwnedEver);
            // The restart is delivered on the release (plus the debounce confirmation).
            CHECK(R.RestartAt >= 1000 + Hold);
            CHECK(R.RestartAt <= 1000 + Hold + BLE_RESET_DEBOUNCE_MS + TickMs);
            printf("  %u ms hold  -> %d restart(s), %d clear(s), restart at %u ms (lift %u ms)\n",
                   (unsigned)Hold, R.Restarts, R.Clears, (unsigned)R.RestartAt,
                   (unsigned)(1000 + Hold));
        }
    }

    // 10 s: the full clear, fired while the contact is still down.
    {
        Rig R = RunContact(1000, 10000);
        CHECK(R.Clears == 1);
        CHECK(R.Restarts == 0);
        CHECK(R.ClearAt == 1000 + BLE_RESET_CLEAR_MS);
        printf("  10 s hold   -> %d clear(s) at %u ms, %d restart(s)\n", R.Clears,
               (unsigned)R.ClearAt, R.Restarts);
    }

    // 30 s: still exactly one clear, and the release after it is not a second action.
    {
        Rig R = RunContact(1000, 30000, 1000);
        CHECK(R.Clears == 1);
        CHECK(R.Restarts == 0);
        printf("  30 s hold   -> %d clear(s), %d restart(s)\n", R.Clears, R.Restarts);
    }
}

static void TestLedSequence()
{
    // The LED is owned (and driven lit by the sequencer) from the 2 s mark until the release, and the
    // pin is handed back exactly at the release that fires the restart.
    {
        Rig R;
        R.Tick(true);
        R.RunUntil(1000 - TickMs, true);
        R.RunUntil(1000 + BLE_RESET_IGNORE_MS - TickMs, false);
        CHECK(!R.PinOwnedEver); // 1.99 s: still nothing
        R.RunUntil(1000 + BLE_RESET_IGNORE_MS, false);
        CHECK(R.PinOwnedEver);  // 2.00 s: the sequencer owns the pin
        CHECK(R.LedLitEver);
        R.RunUntil(1000 + 6000, false);
        CHECK(R.HandBacks == 0); // solid for the whole hold
        R.RunUntil(1000 + 6000 + BLE_RESET_DEBOUNCE_MS + TickMs, true);
        CHECK(R.Restarts == 1);
        CHECK(R.HandBacks == 1); // ... and handed back on the release
        CHECK(!BleResetPinOwned(R.S.Phase));
    }

    // After a clear the LED blinks five times: five lit phases, 250 ms each, one 500 ms period,
    // 2500 ms in total - regardless of the contact still being held (this contact is down from
    // 1.0 s to 15.0 s, i.e. through the clear at 11.0 s and the whole blink sequence).
    {
        Rig R = RunContact(1000, 14000, 2000);
        CHECK(R.Clears == 1);
        CHECK(R.Restarts == 0);
        CHECK(R.BlinkLitRuns == (int)BLE_RESET_BLINK_COUNT);
        CHECK(R.BlinkLitMs >= BLE_RESET_BLINK_COUNT * BLE_RESET_BLINK_HALF_MS - TickMs);
        CHECK(R.BlinkLitMs <= BLE_RESET_BLINK_COUNT * BLE_RESET_BLINK_HALF_MS + TickMs);
        CHECK(R.HandBacks == 1);
        CHECK(!BleResetPinOwned(R.S.Phase));
        printf("  clear       -> %d blinks, %u ms lit (half %u ms, total %u ms), %d hand-back(s)\n",
               R.BlinkLitRuns, (unsigned)R.BlinkLitMs, (unsigned)BLE_RESET_BLINK_HALF_MS,
               (unsigned)BLE_RESET_BLINK_TOTAL_MS, R.HandBacks);
    }

    // The blink clock is the sequence's own: it starts when the device reports the clear complete and
    // runs forward from there, so there are nine edges (five lit phases) and the sequence is over
    // exactly 2500 ms later.
    {
        BleResetHold S;
        uint32_t Now = 0;
        BleResetStep(S, true, Now); // boot level: released
        for (Now = TickMs; Now <= 1000 - TickMs; Now += TickMs)
            BleResetStep(S, true, Now);
        for (; Now <= 1000 + BLE_RESET_CLEAR_MS; Now += TickMs)
        {
            const BleResetOutput Out = BleResetStep(S, false, Now);
            if (Out.Action == BleResetAction::ClearBondsAndRestart)
            {
                CHECK(Now == 1000 + BLE_RESET_CLEAR_MS);
                BleResetClearComplete(S, Now);
            }
        }
        // Half-phase boundaries: lit at +0, dark at +250, lit at +500, ... dark at +2500 (end).
        int Transitions = 0;
        bool Prev = S.LedLit;
        const uint32_t BlinkStart = 1000 + BLE_RESET_CLEAR_MS;
        CHECK(Prev); // the first half is ON
        for (Now += TickMs; Now <= BlinkStart + BLE_RESET_BLINK_TOTAL_MS + TickMs; Now += TickMs)
        {
            const BleResetOutput Out = BleResetStep(S, false, Now);
            if (Out.LedLit != Prev)
                Transitions++;
            Prev = Out.LedLit;
        }
        CHECK(Transitions == (int)(2 * BLE_RESET_BLINK_COUNT - 1)); // 5 blinks = 9 edges
        CHECK(!BleResetPinOwned(S.Phase));
        CHECK(!S.LedLit);
    }
}

static void TestBootTimeLowIsIgnored()
{
    // A pin that is LOW when the firmware starts (a contact held across the reset, or the strapping
    // pin) is not a hold: the timer only starts after a release has been seen. Holding it for 30 s
    // from boot must do nothing.
    Rig R;
    R.Tick(false); // the first sample IS the boot level
    CHECK(!R.S.ReleasedSeen);
    R.RunUntil(30000, false);
    CHECK(R.Restarts == 0);
    CHECK(R.Clears == 0);
    CHECK(!R.PinOwnedEver);

    // Release, then a 3 s contact: that one is a hold.
    R.RunUntil(30500, true);
    CHECK(R.S.ReleasedSeen);
    R.RunUntil(33500, false);
    R.RunUntil(34000, true);
    CHECK(R.Restarts == 1);
    CHECK(R.Clears == 0);
    printf("  boot low    -> %d action(s) in 30 s, then a 3 s hold restarts: %d\n", R.Restarts,
           R.Restarts == 1);
}

static void TestSequenceRearmsOnlyOnRelease()
{
    // A sequence that ends with the contact still down (the blinks run while it is held) must not
    // start a second hold from the same contact: the level has to be released once more.
    Rig R;
    R.Tick(true);
    R.RunUntil(1000, true);
    R.RunUntil(20000, false); // 19 s: the clear at 10 s, the blinks around it, still held
    CHECK(R.Clears == 1);
    CHECK(R.Restarts == 0); // no second action from the same contact
    R.RunUntil(21000, true);
    R.RunUntil(23500, false); // a fresh 2.5 s hold
    R.RunUntil(24000, true);
    CHECK(R.Restarts == 1);
    CHECK(R.Clears == 1);
}

static void TestButtonSuppression()
{
    // 1. The normal path: the sequencer is idle, so field 0 follows the GPIO2 line - a press (line
    //    low) is a press, a released line is not.
    CHECK(ButtonStateFrom(/*Gpio2High=*/false, /*LedOn=*/false, /*ResetOwns=*/false));
    CHECK(!ButtonStateFrom(/*Gpio2High=*/true, false, false));

    // 2. A 10 s hold on the reset pad IS 10 s of the sequencer driving the shared line low, so the
    //    button's own action (the emote reset to Normal) must never be reported. Every pass from the
    //    2 s mark to the clear must leave the sequencer owning the pin, and the report false.
    {
        Rig R;
        R.Tick(true);
        R.RunUntil(1000, true);
        bool OwnedEveryPassAfterIgnore = true;
        bool PressReportedDuringHold = false;
        uint32_t FirstOwnedMs = 0;
        uint32_t FirstUnOwnedMs = 0;
        for (uint32_t t = 1000 + TickMs; t <= 1000 + 10100; t += TickMs)
        {
            R.RunUntil(t, false);
            const bool Owned = BleResetPinOwned(R.S.Phase);
            const bool Lit = R.S.LedLit;
            if (Owned)
            {
                if (FirstOwnedMs == 0)
                    FirstOwnedMs = t;
            }
            else if (FirstOwnedMs != 0 && FirstUnOwnedMs == 0)
            {
                FirstUnOwnedMs = t; // the first pass after the 2 s mark that is NOT owned
                OwnedEveryPassAfterIgnore = false;
            }
            // The pin the button would read: driven LOW by this sequencer while it is lit.
            const bool Gpio2High = !(Owned && Lit);
            if (ButtonStateFrom(Gpio2High, /*LedOn=*/false, Owned))
                PressReportedDuringHold = true;
        }
        CHECK(R.Clears == 1);              // the 10 s hold really is the full clear
        CHECK(OwnedEveryPassAfterIgnore);
        CHECK(FirstOwnedMs >= 1000);
        CHECK(FirstOwnedMs <= 1000 + BLE_RESET_IGNORE_MS + TickMs);
        CHECK(FirstUnOwnedMs == 0);
        CHECK(!PressReportedDuringHold);
        printf("  10.1 s hold -> owned from %u ms to the clear (%u ms), button reported: %s (must be no)\n",
               (unsigned)FirstOwnedMs, (unsigned)FirstUnOwnedMs,
               PressReportedDuringHold ? "yes" : "no");
    }

    // 3. After the sequence the pin is back with the notification driver and the button works again:
    //    the sequencer owns nothing, and a press of the real button is reported.
    {
        Rig R = RunContact(1000, 10000, 6000);
        CHECK(R.Clears == 1);
        CHECK(R.HandBacks == 1);
        CHECK(!BleResetPinOwned(R.S.Phase));
        CHECK(ButtonStateFrom(/*Gpio2High=*/false, /*LedOn=*/false, BleResetPinOwned(R.S.Phase)));
        CHECK(!ButtonStateFrom(/*Gpio2High=*/true, false, BleResetPinOwned(R.S.Phase)));
        CHECK(R.Restarts == 0); // ... and the press is not a reset
        printf("  after clear -> sequencer idle: %s, a press reported again: %s\n",
               BleResetPinOwned(R.S.Phase) ? "no" : "yes",
               ButtonStateFrom(false, false, BleResetPinOwned(R.S.Phase)) ? "yes" : "no");
    }

    // 4. A contact on the GPIO8 reset pad is not a button press: the button is GPIO2, which this
    //    contact does not touch - the line stays high, and field 0 stays false. This holds for a
    //    contact shorter than the ignore window too (nothing is owned, nothing is driven).
    {
        Rig R = RunContact(1000, 300, 500); // a 0.3 s tap on the reset pad
        CHECK(R.Restarts == 0);
        CHECK(R.Clears == 0);
        CHECK(!R.PinOwnedEver);
        CHECK(!ButtonStateFrom(/*Gpio2High=*/true, /*LedOn=*/false, BleResetPinOwned(R.S.Phase)));
    }
}

int main()
{
    printf("### GPIO8 BLE reset: timing table, LED sequence, button suppression\n");
    TestTimingTable();
    TestLedSequence();
    TestBootTimeLowIsIgnored();
    TestSequenceRearmsOnlyOnRelease();
    TestButtonSuppression();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
