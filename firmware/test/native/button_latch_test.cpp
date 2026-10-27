// Host-side test for the Button block's press capture (Core/Devices/ButtonSampling.h), the
// decision the DAS button runs on every main-loop pass.
//
// The defect this pins: the block sampled the pin once per pass and reported the pin's level,
// so a tap that began and ended inside a loop stall was never a transition at all - no counter
// change, no edge update, nothing for the parity or the script to recover. A slow press worked
// and a quick tap vanished, which is exactly the "the emote switch freezes occasionally" that
// was measured on the rig. The fix latches the falling edge in hardware (EXTI line 0's pending
// bit on the DAS) and reports `captured || pin low`, consuming the capture, so one tap is one
// press no matter how long the loop took.
//
// What is under test is the real function: ButtonSample is the device's decision (Button.h calls
// it), and the provider's accounting is the real SubEdgeClassify (SubscriptionsProvider.h calls
// it). Only the loop and the line are modelled here - a list of passes and a list of presses -
// and `Capture = false` models the old level-only sampler so the defect itself is a case in the
// suite rather than a claim in a comment.
//
// The counter arithmetic that follows from parity: a tap is ONE press and ONE lift, i.e. TWO
// counted level changes (the counter counts every level change, the rising trigger only decides
// which of them are sent - SubscriptionsEdge.h). So a tap moves the provider counter by 2 and
// steps the script once; the counter's parity equals the level at every point.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdint>
#include <cstdio>
#include <vector>

#include "Core/Devices/ButtonSampling.h"
#include "Core/Services/SubscriptionsEdge.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
                        #got, #want, g, w);                                          \
        }                                                                            \
    } while (0)

// A press: the switch holds the line low over [Start, End) in microseconds.
struct Pulse {
    uint32_t Start;
    uint32_t End;
};

// What the provider saw over the whole run.
struct Seen {
    int Presses;  // passes the provider counted as a press (Step.Due under EdgeRising)
    int Counted;  // the provider's counter advance: every level change
    bool Level;   // the level the block reports on the last pass
};

static bool LineLow(const std::vector<Pulse> &Pulses, uint32_t T)
{
    for (const Pulse &P : Pulses)
        if (T >= P.Start && T < P.End) return true;
    return false;
}

// One modelled run of the main loop. `Passes` are the times the loop calls DasButtonUpdate();
// `Capture` says whether the device has the hardware edge capture (Button.h) or is the old
// level-only sampler.
static Seen Drive(const std::vector<Pulse> &Pulses, const std::vector<uint32_t> &Passes, bool Capture)
{
    bool Pending = false;  // the hardware capture, EXTI->INTFR bit 0
    bool LastBool = false; // ProviderEntry::LastBool
    uint32_t Counter = 0;  // ProviderEntry::Hash
    int Presses = 0;
    uint32_t PrevT = 0;
    for (const uint32_t T : Passes) {
        // The edge detector runs in hardware: every falling edge of the interval is latched.
        if (Capture)
            for (const Pulse &P : Pulses)
                if (P.Start > PrevT && P.Start <= T) Pending = true;
        const bool Level = ButtonSample(Pending, LineLow(Pulses, T));
        Pending = false; // the device clears the capture on every pass (Button.h), which is what
                         // makes a captured edge worth exactly one press
        const SubEdgeStep Step = SubEdgeClassify(TriggerType::EdgeRising, Level, LastBool);
        LastBool = Level;
        if (Step.Counted) Counter++;
        if (Step.Due) Presses++;
        PrevT = T;
    }
    return Seen{Presses, (int)Counter, LastBool};
}

// A loop that never stalls: one pass every `PeriodUs`.
static std::vector<uint32_t> FastLoop(uint32_t UntilUs, uint32_t PeriodUs = 2000)
{
    std::vector<uint32_t> Passes;
    for (uint32_t T = PeriodUs; T <= UntilUs; T += PeriodUs) Passes.push_back(T);
    return Passes;
}

// The same loop, with the passes inside [StallStartUs, StallStartUs + StallLenUs) missing - a
// CSMA wait, a flash write, the LED bit-bang: the loop is simply not there for that long.
static std::vector<uint32_t> StalledLoop(uint32_t UntilUs, uint32_t StallStartUs, uint32_t StallLenUs,
                                         uint32_t PeriodUs = 2000)
{
    std::vector<uint32_t> Passes;
    for (uint32_t T = PeriodUs; T <= UntilUs; T += PeriodUs)
        if (T < StallStartUs || T >= StallStartUs + StallLenUs) Passes.push_back(T);
    return Passes;
}

// `Taps` presses of `TapUs`, the first starting at `FirstStartUs`, then one every `SpacingUs`.
static std::vector<Pulse> Taps(int Taps, uint32_t TapUs, uint32_t FirstStartUs, uint32_t SpacingUs)
{
    std::vector<Pulse> Pulses;
    for (int i = 0; i < Taps; i++) {
        const uint32_t Start = FirstStartUs + (uint32_t)i * SpacingUs;
        Pulses.push_back(Pulse{Start, Start + TapUs});
    }
    return Pulses;
}

int main()
{
    std::printf("== the pass rule itself ==\n");
    // A captured press is reported whether or not the line is still low; a free line reports
    // lifted. (The capture is consumed by the device's clear, see Drive/run.sh.)
    CHECK(ButtonSample(true, false));
    CHECK(ButtonSample(false, true)); // still held: the capture was consumed already
    CHECK(ButtonSample(true, true));  // captured while held: one report, not two
    CHECK(!ButtonSample(false, false));
    // Arming the capture must not invent a press of its own: a fresh device with a free line
    // reports lifted on the first pass.
    const Seen Idle = Drive({}, FastLoop(100000), true);
    CHECK_EQ(Idle.Presses, 0);
    CHECK_EQ(Idle.Counted, 0);
    CHECK(!Idle.Level);

    std::printf("== a tap shorter than the loop is a press (the reported defect) ==\n");
    // 1 ms tap, entirely inside a 250 ms stall. The old level-only sampler never saw a
    // transition at all; the capture reports exactly one press.
    {
        const std::vector<Pulse> Pulses = Taps(1, 1000, 50000, 0);
        const std::vector<uint32_t> Passes = StalledLoop(400000, 40000, 300000);
        const Seen Old = Drive(Pulses, Passes, false);
        CHECK_EQ(Old.Presses, 0); // the defect: a lost press, nothing for anyone to recover
        CHECK_EQ(Old.Counted, 0);
        const Seen New = Drive(Pulses, Passes, true);
        CHECK_EQ(New.Presses, 1);
        CHECK_EQ(New.Counted, 2); // one press + one lift
        CHECK(!New.Level);
        CHECK_EQ(SubEdgeLevel((uint16_t)New.Counted), New.Level);
    }

    std::printf("== 20 taps of 20 ms: 20 presses, whatever the loop is doing ==\n");
    {
        // Fast loop: the normal case, and the capture must not change it.
        const std::vector<Pulse> Pulses = Taps(20, 20000, 30000, 200000);
        const Seen Fast = Drive(Pulses, FastLoop(4200000), true);
        const Seen FastOld = Drive(Pulses, FastLoop(4200000), false);
        CHECK_EQ(Fast.Presses, 20);
        CHECK_EQ(Fast.Counted, 40);
        CHECK(!Fast.Level);
        // same answer without the capture - the latch adds nothing to a loop that keeps up,
        // so a fast loop cannot start double-counting a press
        CHECK_EQ(FastOld.Presses, Fast.Presses);
        CHECK_EQ(FastOld.Counted, Fast.Counted);

        // The same 20 taps (400 ms apart, so each stall covers one tap and no other), each one
        // entirely inside its own 250 ms stall: still 20 presses, and the old sampler still
        // loses every one of them.
        const std::vector<Pulse> Spaced = Taps(20, 20000, 100000, 400000);
        std::vector<uint32_t> Passes;
        for (int i = 0; i < 20; i++) {
            const uint32_t Start = 100000 + (uint32_t)i * 400000;
            const std::vector<uint32_t> One = StalledLoop(Start + 300000, Start - 10000, 250000);
            for (const uint32_t T : One)
                if (Passes.empty() || T > Passes.back()) Passes.push_back(T);
        }
        const Seen Stalled = Drive(Spaced, Passes, true);
        const Seen StalledOld = Drive(Spaced, Passes, false);
        CHECK_EQ(Stalled.Presses, 20);
        CHECK_EQ(Stalled.Counted, 40);
        CHECK(!Stalled.Level);
        CHECK_EQ(StalledOld.Presses, 0);
        CHECK_EQ(StalledOld.Counted, 0);
    }

    std::printf("== 20 taps of 1 ms: the same 20 presses ==\n");
    {
        const std::vector<Pulse> Pulses = Taps(20, 1000, 30000, 200000);
        const Seen New = Drive(Pulses, FastLoop(4200000), true);
        CHECK_EQ(New.Presses, 20);
        CHECK_EQ(New.Counted, 40);
        CHECK(!New.Level);
    }

    std::printf("== a held press is ONE press ==\n");
    {
        // 200 ms held, sampled by a 2 ms loop: many passes at the same level, one press.
        const std::vector<Pulse> Short = Taps(1, 200000, 50000, 0);
        const Seen A = Drive(Short, FastLoop(400000), true);
        CHECK_EQ(A.Presses, 1);
        CHECK_EQ(A.Counted, 2);
        CHECK(!A.Level);
        // ...and the same press with a stall in the middle of it: the level is still low when
        // the loop comes back, so the stall changes nothing.
        const Seen B = Drive(Short, StalledLoop(400000, 60000, 250000), true);
        CHECK_EQ(B.Presses, 1);
        CHECK_EQ(B.Counted, 2);
        CHECK(!B.Level);
        // 1 s held: identical.
        const std::vector<Pulse> Long = Taps(1, 1000000, 50000, 0);
        const Seen C = Drive(Long, FastLoop(1200000), true);
        CHECK_EQ(C.Presses, 1);
        CHECK_EQ(C.Counted, 2);
        CHECK(!C.Level);
        // the old sampler agrees for a held press - it is the SHORT ones it cannot see
        const Seen D = Drive(Long, FastLoop(1200000), false);
        CHECK_EQ(D.Presses, 1);
        CHECK_EQ(D.Counted, 2);
    }

    std::printf("== the latch is one bit: two taps inside one stall are one press ==\n");
    {
        // Documented limit of a pending-bit capture (a per-edge count would need an ISR and RAM
        // the DAS does not have): the hardware keeps "an edge happened", not how many.
        const std::vector<Pulse> Pulses = Taps(2, 1000, 45000, 30000);
        const Seen New = Drive(Pulses, StalledLoop(400000, 40000, 200000), true);
        CHECK_EQ(New.Presses, 1);
        CHECK_EQ(New.Counted, 2);
        CHECK(!New.Level);
    }

    std::printf("== parity still carries the level after every run ==\n");
    {
        // Coarse sweep: any number of taps, any tap length, a loop with and without stalls - the
        // counter the provider ends on must have the block's own level as its parity, and each
        // tap must have cost exactly two counted level changes.
        for (int n = 0; n <= 8; n++) {
            for (uint32_t TapUs = 1; TapUs <= 500; TapUs *= 8) {
                const std::vector<Pulse> Pulses = Taps(n, TapUs, 30000, 40000);
                const std::vector<uint32_t> Passes = FastLoop(2000000, 3000);
                const Seen New = Drive(Pulses, Passes, true);
                CHECK_EQ(New.Counted, 2 * n);
                CHECK_EQ(SubEdgeLevel((uint16_t)New.Counted), New.Level);
                if (n > 0) CHECK_EQ(New.Presses, n);
            }
        }
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
