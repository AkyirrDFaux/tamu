// Host-side test for the transmit stint's bound (card t_45490cbe).
//
// The defect class this covers: a bus send runs on the device's MAIN task - on the core the same
// task that runs AppInterface.Pump, the display, the scripts and the radios - and it used to be
// able to hold that task for ~770 ms (3 attempts x [100 ms silence + 20 ms sync echo + 20 ms
// first-chunk echo + 100 ms tx-done + up to 16 ms backoff]). The app is mid-transaction for the
// whole of it, so a long stall is a reply that shows up late or not at all.
//
// Two things answer the card's questions, and both are measured here rather than restated:
//
//   * how long a send may block, and
//   * how long the app link may go UNSERVED inside that send.
//
// The waits themselves are the device's real code (Core/Functions/Bus.h: RS485WaitForSilence,
// RS485WaitTicking, RS485SleepTicking - the same functions the core calls, with the chip-specific
// calls injected). The test supplies a virtual microsecond clock, a "link tick" that records when it
// ran, and blocking calls that advance the clock the way uart_read_bytes / uart_wait_tx_done do on
// the device. So a gap between two ticks here is a gap between two RS485AppLinkTick calls there.
//
// What is NOT covered: the echo comparison and the CSMA/CD decision (they are checked where they
// live - the RX-keep test and the bus-frame guards), and any behaviour of the radio itself. The
// stint's attempt structure below mirrors RS485TransmitVerified (Devices/Tamu_v2.0A/RSBus.h); the
// echo windows are modelled as the blocking reads they are, which is the span this test measures.
#include <cstdint>
#include <cstdio>

#include "Blocks/DeviceInfo.h" // DeviceStatusStruct, which Packet.h forward-uses
#include "Core/Functions/Bus.h"

#include "check.h"

#define CHECK_LE(got, limit)                                                         \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), l = (long long)(limit);                \
        if (g > l) {                                                                 \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s = %lld, want <= %lld\n", __FILE__, __LINE__, \
                        #got, g, l);                                                 \
        }                                                                            \
    } while (0)

// A virtual microsecond clock plus the app-link tick the waits call. On the device the tick is
// RS485AppLinkTick (moves queued reply bytes, ticks the radios, dispatches nothing) and the sleeps
// are vTaskDelay; "the link was served" means exactly the same thing in both places: the tick ran.
// MaxGapUs is the longest the link went unserved, which is the number the card asks for.
struct WaitProbe
{
    uint32_t NowUs = 0;
    uint32_t Ticks = 0;
    uint32_t LastTickUs = 0;
    uint32_t MaxGapUs = 0;

    void Reset()
    {
        NowUs = 0;
        Ticks = 0;
        LastTickUs = 0;
        MaxGapUs = 0;
    }

    // A call that sleeps and serves nothing - uart_read_bytes waiting for an echo that never comes,
    // or uart_wait_tx_done's own timeout slice.
    void Block(uint32_t Ms) { NowUs += Ms * 1000u; }

    // A call that serves the link and returns at once (the device's sleeps advance through
    // vTaskDelay, so time moves separately from the tick).
    void Tick()
    {
        const uint32_t Gap = NowUs - LastTickUs;
        if (Gap > MaxGapUs)
            MaxGapUs = Gap;
        LastTickUs = NowUs;
        Ticks++;
    }
};

// A line that is busy for the whole window used to cost a full 100 ms of the silence wait (and
// still can, inside the stint's budget). What matters is that the wait ends at its cap and that the
// link was served the whole time: the yield is the tick.
static void TestSilenceWaitTicksWhileTheBusIsBusy()
{
    WaitProbe P;
    RS485WaitForSilence(0,
                        [](void) -> bool { return true; }, // the line never goes quiet
                        [&P](void) -> uint32_t { return P.NowUs; },
                        [&P](void) {
                            P.NowUs += 1000u; // the device's 1 ms yield
                            P.Tick();
                        },
                        RS485_TX_STINT_MS);

    CHECK(P.NowUs >= RS485_TX_STINT_MS * 1000u);      // it did use the whole budget
    CHECK_LE(P.NowUs, RS485_TX_STINT_MS * 1000u + 1000u); // ... and not a millisecond more
    CHECK(P.Ticks >= RS485_TX_STINT_MS);              // one tick per yield, i.e. per ms
    CHECK_LE(P.MaxGapUs, 1000u);                      // never unserved for longer than one yield
    std::printf("txstint: busy line, 300 ms silence budget -> %u ticks, longest unserved %u us\n",
                (unsigned)P.Ticks, (unsigned)P.MaxGapUs);
}

// The wait must still be a silence wait: once the line quiets it returns one silence window later,
// not at the cap. This is the RS485 timing the card says must not regress - a wait that always ran
// to its deadline would be a 100 ms delay in front of every frame.
static void TestSilenceWaitReturnsAsSoonAsTheLineIsQuiet()
{
    WaitProbe P;
    const uint32_t QuietAtUs = 40 * 1000u;
    RS485WaitForSilence(0,
                        [&P, QuietAtUs](void) -> bool { return P.NowUs < QuietAtUs; },
                        [&P](void) -> uint32_t { return P.NowUs; },
                        [&P](void) {
                            P.NowUs += 1000u;
                            P.Tick();
                        },
                        RS485_TX_STINT_MS);

    CHECK(P.NowUs >= QuietAtUs);
    CHECK_LE(P.NowUs, QuietAtUs + 3 * 1000u); // the window is 176-264 us; a poll or two of slack
    CHECK_LE(P.MaxGapUs, 1000u);
}

// uart_wait_tx_done() blocks for its whole timeout, so the tx-done wait polls it in
// RS485_TX_DONE_SLICE_MS slices and ticks between them. Two facts: it ends at
// RS485_TX_DONE_TIMEOUT_MS (not the 100 ms it used to be), and the link is served once per poll.
static void TestTxDoneWaitIsBoundedAndTicked()
{
    WaitProbe P;
    const bool Done = RS485WaitTicking(
        [&P](void) -> bool {
            P.Block(RS485_TX_DONE_SLICE_MS); // uart_wait_tx_done(2 ms) times out
            return false;
        },
        [&P](void) -> uint32_t { return P.NowUs; },
        [&P](void) { P.Tick(); },
        [&P](uint32_t Ms) { P.NowUs += Ms * 1000u; }, // SliceMs 0: the poll itself blocked
        RS485_TX_DONE_TIMEOUT_MS, 0);

    CHECK(!Done);
    CHECK(P.NowUs >= RS485_TX_DONE_TIMEOUT_MS * 1000u);
    CHECK_LE(P.NowUs, RS485_TX_DONE_TIMEOUT_MS * 1000u + RS485_TX_DONE_SLICE_MS * 1000u);
    CHECK(P.Ticks >= RS485_TX_DONE_TIMEOUT_MS / RS485_TX_DONE_SLICE_MS - 1);
    CHECK_LE(P.MaxGapUs, RS485_TX_DONE_SLICE_MS * 1000u);

    // The other half: the last bit leaving the wire early must not cost the link a tick at all, and
    // must return the moment the driver says it is done.
    WaitProbe Q;
    const bool Early = RS485WaitTicking(
        [](void) -> bool { return true; },
        [&Q](void) -> uint32_t { return Q.NowUs; },
        [&Q](void) { Q.Tick(); },
        [&Q](uint32_t Ms) { Q.NowUs += Ms * 1000u; },
        RS485_TX_DONE_TIMEOUT_MS, 0);
    CHECK(Early);
    CHECK_EQ(Q.NowUs, 0u);
    CHECK_EQ(Q.Ticks, 0u);

    std::printf("txstint: tx-done wait -> %u ms, %u ticks, longest unserved %u us\n",
                (unsigned)(P.NowUs / 1000u), (unsigned)P.Ticks, (unsigned)P.MaxGapUs);
}

// The retry backoff used to be one vTaskDelay() of up to 33 ms with the link stopped for all of it.
// Now it sleeps in RS485_BACKOFF_SLICE_MS slices, so the link is served at least that often. The
// budget constant is checked against the draw it is supposed to bound, so the two cannot drift.
static void TestBackoffSleepIsSlicedAndTheBudgetMatchesTheDraw()
{
    uint32_t DrawMax = 0;
    bool OverBudget = false;
    for (uint16_t Addr = 0; Addr < 64; Addr++)
        for (uint8_t Attempt = 0; Attempt < RS485_RETRIES; Attempt++)
            for (uint32_t Rand = 0; Rand < 1024; Rand += 37)
            {
                const uint32_t Ms = RS485BackoffMs(Addr, Attempt, Rand);
                if (Ms > RS485_BACKOFF_MAX_MS)
                    OverBudget = true;
                if (Ms > DrawMax)
                    DrawMax = Ms;
            }
    CHECK(!OverBudget);
    CHECK_EQ(DrawMax, RS485_BACKOFF_MAX_MS);

    WaitProbe P;
    RS485SleepTicking(
        [&P](void) -> uint32_t { return P.NowUs; },
        [&P](void) { P.Tick(); },
        [&P](uint32_t Ms) { P.NowUs += Ms * 1000u; },
        RS485_BACKOFF_MAX_MS);

    CHECK(P.NowUs >= RS485_BACKOFF_MAX_MS * 1000u);
    CHECK_LE(P.NowUs, RS485_BACKOFF_MAX_MS * 1000u + RS485_BACKOFF_SLICE_MS * 1000u);
    CHECK(P.Ticks >= RS485_BACKOFF_MAX_MS / RS485_BACKOFF_SLICE_MS);
    CHECK_LE(P.MaxGapUs, RS485_BACKOFF_SLICE_MS * 1000u);
    std::printf("txstint: %u ms backoff -> %u ticks, longest unserved %u us\n",
                (unsigned)RS485_BACKOFF_MAX_MS, (unsigned)P.Ticks, (unsigned)P.MaxGapUs);
}

// The stint as RS485TransmitVerified (Devices/Tamu_v2.0A/RSBus.h) runs it: attempts drawn from ONE
// deadline whose leftover also clips the silence window, then a sync echo and a first-chunk echo per
// attempt, a tx-done wait and a retry backoff. The echoes are the two blocking reads (nothing ticks
// inside them - a tick between the sync byte and the first chunk would put a gap inside the frame a
// receiver reads as its end). Three line shapes, all ending without a successful send:
//   (a) a quiet bus whose echo never comes back: every attempt collides on its sync byte;
//   (b) a bus that never goes quiet: every attempt waits out its silence window;
//   (c) quiet, then busy, busy: the first attempt is quick, so the deadline still leaves two busy
//       attempts - this is the shape that reaches the stint's worst case, because the last attempt
//       starts with budget left and then spends all of an attempt's work on top of it.
static void TestStintWorstCaseKeepsTheLinkServed()
{
    // One bit per attempt: set = the line is busy for that attempt's silence window.
    const uint8_t Shapes[] = {0x0, 0x7, 0x6};
    const char *Names[] = {"quiet bus, echo lost", "bus never quiet", "quiet, then busy"};
    const uint32_t UnservedLimitUs = (2 * RS485_ECHO_TIMEOUT_MS + RS485_TX_DONE_SLICE_MS) * 1000u;
    const uint32_t StintLimitMs = RS485_TX_STINT_MS + 2 * RS485_ECHO_TIMEOUT_MS +
                                  RS485_TX_DONE_TIMEOUT_MS + RS485_BACKOFF_MAX_MS;
    uint32_t WorstTotalUs = 0;
    uint32_t WorstGapUs = 0;

    for (unsigned Shape = 0; Shape < sizeof(Shapes) / sizeof(Shapes[0]); Shape++)
    {
        WaitProbe P;
        const uint32_t StintStart = P.NowUs;

        for (int Attempt = 0; Attempt < RS485_RETRIES; Attempt++)
        {
            const uint32_t SpentMs = (P.NowUs - StintStart) / 1000u;
            if (SpentMs >= RS485_TX_STINT_MS)
                break;
            const uint32_t LeftMs = RS485_TX_STINT_MS - SpentMs;
            const uint32_t CapMs =
                LeftMs < RS485_SILENCE_TIMEOUT_MS ? LeftMs : RS485_SILENCE_TIMEOUT_MS;
            const bool Busy = ((Shapes[Shape] >> Attempt) & 1u) != 0;

            RS485WaitForSilence(0,
                                [Busy](void) -> bool { return Busy; },
                                [&P](void) -> uint32_t { return P.NowUs; },
                                [&P](void) {
                                    P.NowUs += 1000u;
                                    P.Tick();
                                },
                                CapMs);

            P.Block(RS485_ECHO_TIMEOUT_MS); // sync byte: its echo never arrives
            P.Block(RS485_ECHO_TIMEOUT_MS); // first chunk: same

            (void)RS485WaitTicking(
                [&P](void) -> bool {
                    P.Block(RS485_TX_DONE_SLICE_MS);
                    return false;
                },
                [&P](void) -> uint32_t { return P.NowUs; },
                [&P](void) { P.Tick(); },
                [&P](uint32_t Ms) { P.NowUs += Ms * 1000u; },
                RS485_TX_DONE_TIMEOUT_MS, 0);

            RS485SleepTicking(
                [&P](void) -> uint32_t { return P.NowUs; },
                [&P](void) { P.Tick(); },
                [&P](uint32_t Ms) { P.NowUs += Ms * 1000u; },
                RS485_BACKOFF_MAX_MS);
        }

        const uint32_t TotalUs = P.NowUs - StintStart;
        if (TotalUs > WorstTotalUs)
            WorstTotalUs = TotalUs;
        if (P.MaxGapUs > WorstGapUs)
            WorstGapUs = P.MaxGapUs;

        std::printf("txstint: %-20s -> %u ms send, %u link ticks, longest unserved %u us\n",
                    Names[Shape], (unsigned)(TotalUs / 1000u), (unsigned)P.Ticks,
                    (unsigned)P.MaxGapUs);

        CHECK(P.Ticks > 0);
        // The card's rule, as an assertion: whatever the line does, the send never stops the app
        // link for longer than the two echo windows and one tx-done poll...
        CHECK_LE(P.MaxGapUs, UnservedLimitUs);
        // ... and it does not outlive the deadline plus one attempt's non-wait work.
        CHECK_LE(TotalUs / 1000u, StintLimitMs);
        // The pre-fix worst case was ~770 ms; nothing here may approach it.
        CHECK_LE(TotalUs / 1000u, 400u);
    }

    // Worst of the shapes versus the figure in Bus.h's comment: it is a measurement, not a
    // restatement.
    std::printf("txstint: worst send %u ms (ceiling %u ms), longest unserved stretch %u ms\n",
                (unsigned)(WorstTotalUs / 1000u), (unsigned)StintLimitMs,
                (unsigned)(WorstGapUs / 1000u));
    CHECK_LE(WorstTotalUs / 1000u, StintLimitMs);
    CHECK_LE(WorstGapUs, 50u * 1000u); // the card's 50 ms bound
}

int main()
{
    TestSilenceWaitTicksWhileTheBusIsBusy();
    TestSilenceWaitReturnsAsSoonAsTheLineIsQuiet();
    TestTxDoneWaitIsBoundedAndTicked();
    TestBackoffSleepIsSlicedAndTheBudgetMatchesTheDraw();
    TestStintWorstCaseKeepsTheLinkServed();

    std::printf("%s: %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
