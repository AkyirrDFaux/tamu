// Host-side test for the ACKNOWLEDGED edge update (owner ruling, 2026-10-27): an edge update is
// confirmed too, so one that was lost is RE-SENT instead of vanishing until the next press.
//
// The provider's 32-bit Hash/Hashlike carries both roles for an edge trigger (Docs/Services/
// Subscriptions.md "Provider Table Entry"):
//
//   low 16 bits  - the local counter (the update's payload), advancing once per counted level change
//   high 16 bits - the last counter the requester confirmed
//
// so "unacknowledged" is `high != low` and needs no extra flag. This file drives the real helpers
// (Core/Services/SubscriptionsEdge.h) against the real requester state machine, over a wire that can
// swallow updates and acknowledgements, and asserts the properties the ruling asks for:
//
//   * a normal press            -> exactly one send, and the row goes quiet after the ack
//   * a swallowed update        -> the SAME counter is re-sent at the retry interval until an ack
//                                  arrives, and the pulse count stays the number of edges
//   * three presses in a window -> one send carrying the newest counter (one in-flight counter, no
//                                  queue), and still exactly three pulses
//   * a cancelled row           -> nothing is re-sent
//   * an ack that raced a newer edge -> does not confirm it (the press is re-sent, not dropped)
//
// The device side that cannot be linked on the host (the provider's send, the requester's ack
// packet) is guarded structurally in test/native/run.sh; what is under test here is the accounting
// both ends run.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdint>
#include <cstdio>
#include <vector>

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

// The provider half, exactly as Core/Services/SubscriptionsProvider.h runs it: classify the sample,
// advance the packed word, and send its low half when the halves differ and the retry interval (the
// subscription's minTime) has elapsed.
struct Provider {
    TriggerType Trigger = TriggerType::EdgeRising;
    uint32_t MinTimeMs = 20;
    uint32_t Hash = 0; // SubEdgePackHalves(confirmed, counter)
    bool LastBool = false;
    uint32_t LastSentMs = 0;
    bool Gone = false; // the row was cancelled (ProviderClearEntry)

    // One main-loop pass. Returns true when an update went out, with `Wire` = its payload.
    bool Evaluate(uint32_t NowMs, bool Level, uint16_t *Wire) {
        if (Gone) return false;
        const SubEdgeStep Step = SubEdgeClassify(Trigger, Level, LastBool);
        LastBool = Level;
        if (Step.Counted) Hash = SubEdgeAdvance(Hash, Step.Due);
        const bool Send = SubEdgeUnacked(Hash) && ((NowMs - LastSentMs) >= MinTimeMs);
        if (!Send) return false;
        LastSentMs = NowMs;
        uint8_t Payload[SUB_EDGE_COUNTER_SIZE];
        SubEdgeCounterStore(Payload, SubEdgeLowHalf(Hash)); // ProviderEdgePayload
        *Wire = SubEdgeCounterLoad(Payload);                // what the requester reads back
        return true;
    }

    // The requester's confirmation arrives (HandleProviderConfirmation's edge branch).
    void Confirm(uint32_t Answer) {
        if (SubEdgeAnswers(Hash, Answer)) Hash = Answer;
    }

    uint16_t Counter() const { return SubEdgeLowHalf(Hash); }
    uint16_t Confirmed() const { return SubEdgeHighHalf(Hash); }
};

// The requester half: the shipped SubEdgeState, one pass of the pulse per main-loop pass.
struct Requester {
    SubEdgeState Edge;
    unsigned Pulses = 0; // true passes written to the target

    void Push(TriggerType Trigger, uint16_t Counter) { Edge.Arrive(Trigger, Counter, false); }

    void Pass() {
        const SubEdgeState::SubPulseStep Step = Edge.NextPass();
        if (Step.Write && Step.Value) Pulses++;
    }
};

struct Press {
    uint32_t Down, Up;
};

struct Link {
    int DropUpdates = 0; // swallow this many updates whole (the provider never learns)
    int DropAcks = 0;    // deliver the update, lose its acknowledgement
};

struct Result {
    unsigned Sends = 0;
    unsigned Pulses = 0;
    bool Quiet = false;      // the row ended confirmed (high == low)
    uint16_t LastWire = 0;   // the counter on the last update
    uint16_t Counter = 0;    // the provider's low half at the end
};

// The simulation: a millisecond clock, the provider evaluated every pass (the DAS does), the
// requester's pulse spent every `CoreLoopMs` (the core's measured ~6 ms pass).
static Result Run(TriggerType Trigger, uint32_t MinTimeMs, const std::vector<Press> &Presses,
                  uint32_t EndMs, Link Wire, uint32_t CancelAtMs = 0xFFFFFFFFu) {
    Provider P;
    Requester R;
    P.Trigger = Trigger;
    P.MinTimeMs = MinTimeMs;
    // The row's zero point: the install answer, or the first push it ever took.
    R.Edge.Rebaseline(0);
    P.LastSentMs = 0;
    Result Out;
    const uint32_t CoreLoopMs = 6;
    for (uint32_t T = 1; T <= EndMs; T++) {
        if (T == CancelAtMs) P.Gone = true;
        bool Level = false;
        for (const auto &Pr : Presses)
            if (T >= Pr.Down && T < Pr.Up) Level = true;
        uint16_t WireCounter = 0;
        if (P.Evaluate(T, Level, &WireCounter)) {
            Out.Sends++;
            Out.LastWire = WireCounter;
            if (Wire.DropUpdates > 0) {
                Wire.DropUpdates--;
            } else {
                R.Push(Trigger, WireCounter);
                if (Wire.DropAcks > 0) Wire.DropAcks--;
                else P.Confirm(SubEdgeConfirmWord(WireCounter));
            }
        }
        if ((T % CoreLoopMs) == 0) R.Pass();
    }
    Out.Pulses = R.Pulses;
    Out.Quiet = !SubEdgeUnacked(P.Hash);
    Out.Counter = P.Counter();
    return Out;
}

int main() {
    std::printf("== the packed edge state: counter low, last confirmed counter high ==\n");

    // ---- The packing itself: a counted change moves the low half, and the high half only when the
    //      change is one the trigger does not send on an already-confirmed row (silence confirms it).
    {
        uint32_t H = SubEdgePackHalves(5, 5); // confirmed and counter agree: nothing owed
        CHECK(!SubEdgeUnacked(H));
        CHECK_EQ(SubEdgeLowHalf(H), 5);
        CHECK_EQ(SubEdgeHighHalf(H), 5);

        // A DUE change (a press on a rising row) advances the counter alone: the row now owes a send.
        H = SubEdgeAdvance(H, true);
        CHECK_EQ(SubEdgeLowHalf(H), 6);
        CHECK_EQ(SubEdgeHighHalf(H), 5);
        CHECK(SubEdgeUnacked(H));

        // A change the trigger does not send, on a row that owes nothing: both halves move, so the
        // silence is the confirmation and no update is generated.
        H = SubEdgePackHalves(5, 5);
        H = SubEdgeAdvance(H, false);
        CHECK_EQ(SubEdgeLowHalf(H), 6);
        CHECK_EQ(SubEdgeHighHalf(H), 6);
        CHECK(!SubEdgeUnacked(H));

        // The same silent change on a row that DOES owe a send keeps the high half, so the pending
        // send carries the newer counter (a release after an unsent press is not a new send).
        H = SubEdgePackHalves(5, 4); // counter 5, confirmed 4: unacked
        H = SubEdgeAdvance(H, false);
        CHECK_EQ(SubEdgeLowHalf(H), 6);
        CHECK_EQ(SubEdgeHighHalf(H), 4);
        CHECK(SubEdgeUnacked(H));

        // The counter wraps at 2^16 in its own half: the high half is not carried into.
        H = SubEdgePackHalves(0xFFFF, 0xFFFF);
        H = SubEdgeAdvance(H, true);
        CHECK_EQ(SubEdgeLowHalf(H), 0);
        CHECK_EQ(SubEdgeHighHalf(H), 0xFFFF); // untouched by the low half's wrap
        CHECK(SubEdgeUnacked(H));
        H = SubEdgeAdvance(H, false); // silence on an owed row: still owed
        CHECK_EQ(SubEdgeLowHalf(H), 1);
        CHECK_EQ(SubEdgeHighHalf(H), 0xFFFF);

        // The confirmation word is the counter in both halves, and only that counter is confirmed.
        CHECK_EQ(SubEdgeConfirmWord(7), 0x00070007u);
        CHECK_EQ(SubEdgeConfirmWord(0xFFFF), 0xFFFFFFFFu);
        CHECK(SubEdgeAnswers(SubEdgePackHalves(7, 9), SubEdgeConfirmWord(7)));
        CHECK(!SubEdgeAnswers(SubEdgePackHalves(8, 9), SubEdgeConfirmWord(7))); // an older answer
        CHECK(!SubEdgeAnswers(SubEdgePackHalves(7, 9), 0x00080008u));
        CHECK_EQ(SubEdgeConfirmWord(0) >> 16, 0); // and a wrap answers 0, not 0x10000
    }

    std::printf("== the provider re-sends an unacknowledged counter instead of dropping it ==\n");

    // ---- A normal press: one send, and the row is quiet once the ack lands.
    {
        const Result R = Run(TriggerType::EdgeRising, 20, {{100, 200}}, 500, Link{});
        CHECK_EQ(R.Sends, 1);
        CHECK_EQ(R.LastWire, 1); // press: counter 0 -> 1
        CHECK(R.Quiet);
        CHECK_EQ(R.Counter, 2); // the press AND its release are counted; only the press is sent
        CHECK_EQ(R.Pulses, 1);
        std::printf("  normal press: %u send(s), wire counter %u, %u pulse(s), quiet=%d\n", R.Sends,
                    R.LastWire, R.Pulses, (int)R.Quiet);
    }

    // ---- The first TWO updates are swallowed: the provider repeats the SAME counter until one gets
    //      through, and the target still sees exactly one press.
    {
        Link Wire;
        Wire.DropUpdates = 2;
        const Result R = Run(TriggerType::EdgeRising, 20, {{100, 200}}, 500, Wire);
        CHECK_EQ(R.LastWire, 1);   // never incremented by a retry
        CHECK_EQ(R.Sends, 3);      // the press, then two re-sends of the same counter
        CHECK(R.Quiet);            // the third one was acknowledged
        CHECK_EQ(R.Pulses, 1);     // one edge, one pulse - the re-sends are duplicates at the requester
        std::printf("  two updates swallowed: %u send(s), wire counter %u, %u pulse(s), quiet=%d\n",
                    R.Sends, R.LastWire, R.Pulses, (int)R.Quiet);
    }

    // ---- The update arrives but its ACK is lost: the provider repeats the same counter until an ack
    //      lands, and the requester treats every repeat as a duplicate (no extra pulses).
    {
        Link Wire;
        Wire.DropAcks = 2;
        const Result R = Run(TriggerType::EdgeRising, 20, {{100, 200}}, 500, Wire);
        CHECK_EQ(R.Sends, 3);
        CHECK(R.Quiet);
        CHECK_EQ(R.Pulses, 1);
        std::printf("  two acks lost: %u send(s), wire counter %u, %u pulse(s), quiet=%d\n", R.Sends,
                    R.LastWire, R.Pulses, (int)R.Quiet);
    }

    // ---- A dead requester cannot pin the row for ever: the retries continue at the retry interval
    //      only while the provider entry exists, and a cancelled row stops immediately (below).
    {
        Link Wire;
        Wire.DropUpdates = 1000000; // nothing ever arrives
        const Result R = Run(TriggerType::EdgeRising, 20, {{100, 200}}, 2000, Wire);
        // 2000 ms at a 20 ms retry interval: bounded, not one send per loop pass.
        CHECK(R.Sends >= 90);
        CHECK(R.Sends <= 101);
        CHECK(!R.Quiet); // still unacknowledged: no ack will ever come
        CHECK_EQ(R.Pulses, 0);
        std::printf("  2000 ms of a dead requester: %u send(s) at the 20 ms retry interval\n", R.Sends);
    }

    // ---- Nothing is re-sent after the row is cancelled.
    {
        Link Wire;
        Wire.DropUpdates = 1000000;
        const Result R = Run(TriggerType::EdgeRising, 20, {{100, 200}}, 2000, Wire, 300);
        const Result R2 = Run(TriggerType::EdgeRising, 20, {{100, 200}}, 300, Wire); // same prefix
        CHECK_EQ(R2.Sends, 11);      // 100, 120 ... 300 without the cancel
        CHECK_EQ(R.Sends, 10);       // ...and the pass at 300 is already gone
        CHECK(R.Sends >= 5);         // the re-sends before the cancel did happen
        std::printf("  cancelled at 300 ms: %u send(s) in total (would be more without the cancel)\n",
                    R.Sends);
    }

    std::printf("== one in-flight counter, not a queue ==\n");

    // ---- Three presses inside one retry window: ONE send, carrying the newest counter (6 counted
    //      steps = three presses and three releases), and still exactly three pulses.
    {
        const Result R = Run(TriggerType::EdgeRising, 100, {{10, 20}, {30, 40}, {50, 60}}, 800, Link{});
        CHECK_EQ(R.Sends, 1);
        CHECK_EQ(R.LastWire, 6); // three counted level changes in each direction
        CHECK(R.Quiet);
        CHECK_EQ(R.Pulses, 3); // one per press, not one per counter step
        std::printf("  three presses in one window: %u send(s), wire counter %u, %u pulse(s)\n", R.Sends,
                    R.LastWire, R.Pulses);
    }

    // ---- A press that lands while a send is unacknowledged raises the counter, and the next send
    //      carries the higher value: the earlier press is not lost, it is delivered with this one.
    {
        Link Wire;
        Wire.DropUpdates = 1; // the first press's update is lost
        const Result R = Run(TriggerType::EdgeRising, 20, {{100, 200}, {300, 400}}, 900, Wire);
        CHECK_EQ(R.LastWire, 3);  // press, release, press = 3 counted steps
        CHECK_EQ(R.Pulses, 2);    // both presses reach the target, through one update
        CHECK(R.Quiet);
        std::printf("  press lost, press again: wire counter %u, %u pulse(s)\n", R.LastWire, R.Pulses);
    }

    // ---- An ack that raced a newer edge must not confirm it: the newer counter is re-sent, and the
    //      press it carries still reaches the target.
    {
        // Build the race by hand: the provider sends counter 1, a second press raises the counter to
        // 3, and only then does the FIRST update's ack arrive.
        Provider P;
        P.Trigger = TriggerType::EdgeRising;
        P.MinTimeMs = 0;
        uint16_t WireCounter = 0;
        CHECK(P.Evaluate(10, true, &WireCounter)); // press: counter 1, sent
        CHECK_EQ(WireCounter, 1);
        CHECK(P.Evaluate(20, false, &WireCounter)); // release: counter 2, unacked -> sent too
        CHECK_EQ(WireCounter, 2);
        CHECK(P.Evaluate(30, true, &WireCounter)); // press: counter 3
        CHECK_EQ(WireCounter, 3);
        CHECK_EQ(P.Counter(), 3);
        P.Confirm(SubEdgeConfirmWord(1)); // the ack for the FIRST update arrives late
        CHECK(SubEdgeUnacked(P.Hash));    // the newer counter is still unacknowledged
        CHECK_EQ(P.Confirmed(), 0);       // ...and nothing was confirmed by it
        CHECK(P.Evaluate(40, true, &WireCounter)); // so the newer counter is re-sent
        CHECK_EQ(WireCounter, 3);
        P.Confirm(SubEdgeConfirmWord(3));
        CHECK(!SubEdgeUnacked(P.Hash));
        CHECK_EQ(P.Confirmed(), 3);
        // And the requester's own accounting of that stream: three counted steps from the zero point
        // (press, release, press) are two presses on a rising row.
        Requester R;
        R.Edge.Rebaseline(0);
        R.Push(TriggerType::EdgeRising, 1);
        R.Push(TriggerType::EdgeRising, 2);
        R.Push(TriggerType::EdgeRising, 3);
        for (int i = 0; i < 8; i++) R.Pass();
        CHECK_EQ(R.Pulses, 2);
    }

    // ---- A change the trigger does not send never causes an update of its own (a release on a
    //      rising row), however long it sits confirmed.
    {
        const Result R = Run(TriggerType::EdgeRising, 0, {{100, 200}}, 500, Link{});
        CHECK_EQ(R.Sends, 1); // the press only
        // From a lifted start, a press and release count two steps but send once.
        bool Level = false;
        (void)Level;
        const Result R2 = Run(TriggerType::EdgeFalling, 0, {{100, 200}}, 500, Link{});
        CHECK_EQ(R2.Sends, 1);      // a press is not a falling edge; its RELEASE is
        CHECK_EQ(R2.LastWire, 2);   // ...and that update carries both counted steps
        CHECK(R2.Quiet);
        CHECK_EQ(R2.Pulses, 1);
    }

    // ---- The counter is u16 on the wire: a wrap is an ordinary +1 for the requester, and the packed
    //      word keeps the two halves apart across it.
    {
        Provider P;
        P.Trigger = TriggerType::EdgeAny;
        P.MinTimeMs = 0;
        P.Hash = SubEdgePackHalves(0xFFFE, 0xFFFE); // confirmed, counter: two steps before the wrap
        uint16_t WireCounter = 0;
        CHECK(P.Evaluate(10, true, &WireCounter)); // 0xFFFE -> 0xFFFF
        CHECK_EQ(WireCounter, 0xFFFF);
        CHECK(P.Evaluate(20, false, &WireCounter)); // 0xFFFF -> 0x0000: a wrap in the low half only
        CHECK_EQ(WireCounter, 0x0000);
        CHECK_EQ(P.Confirmed(), 0xFFFE); // the high half did not take the carry
        P.Confirm(SubEdgeConfirmWord(0));
        CHECK(!SubEdgeUnacked(P.Hash));
        CHECK_EQ(P.Hash, 0x00000000u); // confirmed 0, counter 0
        std::printf("  wrap through 0xFFFF: wire %u, %u, then confirmed\n", 0xFFFFu, 0x0000u);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
