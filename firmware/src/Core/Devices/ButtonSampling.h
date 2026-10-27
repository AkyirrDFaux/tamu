#pragma once

// What a Button block reports for one main-loop pass.
//
// A Button block is a LEVEL, and the main loop samples it once per pass, so a level-only read
// can only see a press that is still there when the loop arrives: a tap that begins and ends
// inside a loop stall (a CSMA wait, a flash write, the LED bit-bang) is never a transition at
// all - no counter change, no update, nothing for the parity or the script to recover. That is
// the "the switch loses presses at random" family: a slow press works, a quick tap vanishes.
//
// The capture closes that hole. A GPIO edge detector latches the falling edge (a press, active
// low) in HARDWARE and holds it until software consumes it - on the DAS that is EXTI line 0's
// pending bit (Devices/DAS_v0.1/Button.h) - so the edge survives however long the loop took.
//
// The rule, shared by the device and the host test: a pass reports pressed if an edge was
// captured since the previous pass OR the line is low right now, and the capture is consumed by
// the pass that read it. "Consumed once" is what keeps the count honest:
//   - a HELD press is one press: the line stays low, and nothing new is captured while it is low,
//     so the passes in between report the same level and the provider counts nothing;
//   - the RELEASE is the line read going high - the capture only sees falling edges;
//   - so one tap is one press and one lift, two level changes, which is exactly what keeps the
//     provider's counter PARITY equal to the level (Core/Services/SubscriptionsEdge.h: the
//     counter counts every level change, the trigger only decides which ones are sent).
//
// The consume itself is one store in Button.h (clear the pending bit after reading it, on every
// pass - clearing a bit that is not set costs nothing), and test/native/run.sh guards it there:
// this header must stay a decision, and the DAS must clear the capture it just read.
//
// Host-clean on purpose (no device headers, no globals): test/native/button_latch_test.cpp drives
// this same function against a modelled loop, so the decision cannot drift from the device's.
static inline bool ButtonSample(bool PressCaptured, bool PinLow)
{
    return PressCaptured || PinLow;
}
