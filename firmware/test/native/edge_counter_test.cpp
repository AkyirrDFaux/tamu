// Host-side test for the edge counter that IS an edge-triggered value update's payload, and for the
// pulse the requester writes into its target (Core/Services/SubscriptionsEdge.h).
//
// The counter and the target value are two different things:
//   - the COUNTER is the provider's bookkeeping. Its parity is the level (odd = pressed, even =
//     lifted), and the requester only READS it, to learn how many edges are new (the delta).
//   - the TARGET VALUE is the requester's output: a PULSE. An edge update is worth one TRUE loop pass
//     per new edge of the trigger's direction, consecutive, and then one FALSE (owner ruling,
//     2026-10-27). The counter is never written into the target - not as a number, not as a level.
//
// The defect this pins: an edge-triggered update is a *press*, and the wire used to carry the value
// alone - a Bool. A press that arrived and a press that was lost therefore looked identical at the
// requester. Then the wire carried the counter and the requester wrote its parity, which made a press
// readable but left the target TRUE for ever: a held button, or any press, sat at true and every
// consumer had to remember the last level to count a second one.
//
// What is under test is the real state machine the core runs, not a copy of it: RequesterEntry embeds
// SubEdgeState and the requester only calls its methods (Core/Services/SubscriptionsEdge.h), so this
// file drives Arrive/NextPass/GiveUp exactly as Core/Services/SubscriptionsRequester.h does, and
// SubEdgeClassify exactly as Core/Services/SubscriptionsProvider.h does. The classification is:
//   Baseline  - the first counter since the row was installed (or since the provider answered a
//               re-registration): the new zero point, never a gap
//   Duplicate - the same counter again (a verified send retried after a lost ack): ignored
//   Next      - counter + 1, the normal case, and also what a wrap looks like (modular)
//   Gap       - counter + n (n >= 2): n-1 counters were never seen, so edges were lost
//   Resync    - the counter went backwards: a restarted provider (a node's table is RAM-only),
//               counted as a baseline break, never as a jump of ~32k presses
//
// The device side that cannot be linked on the host (the provider's payload build, the requester's
// target write) is guarded structurally in test/native/run.sh.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdint>
#include <cstdio>
#include <cstring>

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

// Samples the target field once per loop pass, the way a consumer script reads it:
//   'T' the pass wrote true     - the field reads true on that loop
//   'F' the pass wrote false    - the field reads false on that loop
//   '.' nothing was written     - the field keeps whatever it had (false, once the pulse is over)
// This is the device's SubscriptionsPulseTargets loop: one NextPass per main-loop pass, one target
// write per step that asks for one.
static void Sample(SubEdgeState &State, char *Out, unsigned Passes) {
    unsigned i = 0;
    for (; i < Passes; i++) {
        const SubEdgeState::SubPulseStep Step = State.NextPass();
        Out[i] = Step.Write ? (Step.Value ? 'T' : 'F') : '.';
    }
    Out[i] = '\0';
}

// The same sampling run, printed: the handoff quotes these lines as the per-loop measurement.
static void PrintRun(const char *What, const char *Run, unsigned Passes) {
    std::printf("  %-34s %u passes: %s\n", What, Passes, Run);
}


int main() {
    // ---- The wire encoding: u16 little-endian, and it is the WHOLE payload.
    {
        CHECK_EQ(SUB_EDGE_COUNTER_SIZE, 2);
        CHECK_EQ(2, sizeof(uint16_t)); // the counter is a u16 on the wire, not a wider value
        uint8_t Wire[8] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
        SubEdgeCounterStore(Wire, 0x1234);
        CHECK_EQ(Wire[0], 0x34); // low byte first
        CHECK_EQ(Wire[1], 0x12);
        CHECK_EQ(Wire[2], 0xEE); // nothing else touched: no value is appended behind the counter
        CHECK_EQ(SubEdgeCounterLoad(Wire), 0x1234);
        // The provider's counter is 32-bit (it increments a uint32): the wire carries its low 16
        // bits and wraps there, so 0x10001 is the same wire value as 0x0001.
        SubEdgeCounterStore(Wire, 0x10001);
        CHECK_EQ(SubEdgeCounterLoad(Wire), 0x0001);
        CHECK_EQ(Wire[2], 0xEE);
        SubEdgeCounterStore(Wire, 0xFFFF);
        CHECK_EQ(SubEdgeCounterLoad(Wire), 0xFFFF);
        // Anything that is not an edge trigger keeps its payload: the counter is edge-only.
        CHECK(!SubTriggerIsEdge(TriggerType::None));
        CHECK(!SubTriggerIsEdge(TriggerType::Periodic));
        CHECK(!SubTriggerIsEdge(TriggerType::OnChangePeriodic));
        CHECK(!SubTriggerIsEdge(TriggerType::OnChangeConfirm));
        CHECK(!SubTriggerIsEdge(TriggerType::DeltaPeriodic));
        CHECK(SubTriggerIsEdge(TriggerType::EdgeRising));
        CHECK(SubTriggerIsEdge(TriggerType::EdgeFalling));
        CHECK(SubTriggerIsEdge(TriggerType::EdgeAny));
    }

    // ---- The counter's parity is the level - a property of the COUNTER, never of the target.
    {
        CHECK(!SubEdgeLevel(0));
        CHECK(SubEdgeLevel(1));
        CHECK(!SubEdgeLevel(2));
        CHECK(SubEdgeLevel(3));
        CHECK(!SubEdgeLevel(0xFFFE));
        CHECK(SubEdgeLevel(0xFFFF));
        // A wrap keeps the meaning: 0xFFFF (odd, pressed) -> 0x0000 (even, lifted) is a release.
        CHECK(SubEdgeLevel(0xFFFF) != SubEdgeLevel(0x0000));
    }

    // ---- The provider's step: EVERY level change moves the counter (parity is the level), and
    //      only the change the trigger was asked about is due.
    {
        SubEdgeStep S = SubEdgeClassify(TriggerType::EdgeRising, true, false);
        CHECK(S.Counted);
        CHECK(S.Due); // a press on a rising subscription
        S = SubEdgeClassify(TriggerType::EdgeRising, false, true);
        CHECK(S.Counted); // the release is counted too ...
        CHECK(!S.Due);    // ... but is not sent

        S = SubEdgeClassify(TriggerType::EdgeFalling, false, true);
        CHECK(S.Counted);
        CHECK(S.Due);
        S = SubEdgeClassify(TriggerType::EdgeFalling, true, false);
        CHECK(S.Counted);
        CHECK(!S.Due);

        S = SubEdgeClassify(TriggerType::EdgeAny, true, false);
        CHECK(S.Counted);
        CHECK(S.Due);
        S = SubEdgeClassify(TriggerType::EdgeAny, false, true);
        CHECK(S.Counted);
        CHECK(S.Due);

        // No change, no counter, nothing due.
        S = SubEdgeClassify(TriggerType::EdgeAny, true, true);
        CHECK(!S.Counted);
        CHECK(!S.Due);
        S = SubEdgeClassify(TriggerType::EdgeRising, false, false);
        CHECK(!S.Counted);
        CHECK(!S.Due);
        // A non-edge trigger never marks a send (the provider only calls this for edge triggers;
        // the definition must not turn some other trigger into one).
        CHECK(!SubEdgeClassify(TriggerType::Periodic, true, false).Due);
    }

    // ---- How many edges of the direction a counter advance contains: the number of true passes an
    //      update is worth. The counter counts both directions, so a rising subscription's press is
    //      every other step.
    {
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeAny, 0, 3), 3);
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeAny, 0xFFFF, 1), 1); // a wrap is one step

        // Rising: from an even counter the first step is the press; from an odd one it is the lift.
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeRising, 0, 1), 1);
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeRising, 0, 2), 1); // press + lift
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeRising, 1, 1), 0); // a lift, not a press
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeRising, 1, 2), 1); // lift + press
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeRising, 1, 3), 1); // one press (at 3)
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeRising, 1, 4), 2); // two presses + a lift
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeRising, 0xFFFF, 2), 1); // wrap: 0, 1

        // Falling is the mirror image.
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeFalling, 1, 1), 1); // a lift
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeFalling, 1, 2), 1); // lift + press
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeFalling, 0, 1), 0); // a press, not a lift
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeFalling, 2, 4), 2); // two lifts

        // No advance, no edges.
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeAny, 7, 0), 0);
        CHECK_EQ(SubEdgeDirectionCount(TriggerType::EdgeRising, 7, 0), 0);
    }

    std::printf("== the target is a pulse: sampled once per loop pass ==\n");

    // ---- A fresh row: the target may hold anything after a boot or a set, so the first pass closes
    //      it with one false and then leaves it alone. No edge, no true.
    {
        SubEdgeState S;
        CHECK(S.High); // a new row clears the target rather than trusting it
        char Run[16];
        Sample(S, Run, 6);
        PrintRun("no edge at all", Run, 6);
        CHECK(std::strcmp(Run, "F.....") == 0);
        CHECK_EQ(S.Budget, 0);
    }

    // ---- One press on a rising row: one true loop, then false, then nothing.
    {
        SubEdgeState S;
        CHECK(S.Arrive(TriggerType::EdgeRising, 40, false) == SubEdgeKind::Baseline);
        CHECK_EQ(S.Budget, 1); // the push that starts the row is one edge: it is still a press
        char Run[16];
        Sample(S, Run, 6);
        PrintRun("single press (baseline push)", Run, 6);
        CHECK(std::strcmp(Run, "TF....") == 0);

        // The next press: the counter went 40 -> 42 (press + release), so it is ONE press and one
        // true pass - not two.
        CHECK(S.Arrive(TriggerType::EdgeRising, 42, false) == SubEdgeKind::Gap);
        CHECK_EQ(S.Missing, 1); // the release, reported but not a press
        CHECK_EQ(S.Budget, 1);
        Sample(S, Run, 6);
        PrintRun("single press (counter +2)", Run, 6);
        CHECK(std::strcmp(Run, "TF....") == 0);
    }

    // ---- A held button: one press, one true loop, and then false however long it is held. Holding
    //      produces no further counter changes, so nothing re-arms the pulse.
    {
        SubEdgeState S;
        S.Arrive(TriggerType::EdgeRising, 0, false); // baseline
        S.NextPass();
        S.NextPass();                                // its own pulse is over
        CHECK(S.Arrive(TriggerType::EdgeRising, 1, false) == SubEdgeKind::Next); // the press
        CHECK_EQ(S.Budget, 1);
        char Run[32];
        Sample(S, Run, 20);
        PrintRun("held button (one edge, held)", Run, 20);
        CHECK(std::strcmp(Run, "TF..................") == 0);
        CHECK_EQ(S.Budget, 0);
        CHECK(!S.High); // the field is false and the row has stopped writing to it
    }

    // ---- A burst of 3 edges in one update: exactly three true loops, then false - not one long
    //      true, and not one true.
    {
        SubEdgeState S;
        S.Arrive(TriggerType::EdgeAny, 10, false); // baseline
        S.NextPass();
        S.NextPass(); // settle on false first
        CHECK(S.Arrive(TriggerType::EdgeAny, 13, false) == SubEdgeKind::Gap);
        CHECK_EQ(S.Budget, 3);
        char Run[16];
        Sample(S, Run, 8);
        PrintRun("burst of 3 (EdgeAny, delta 3)", Run, 8);
        CHECK(std::strcmp(Run, "TTTF....") == 0);

        // The same three presses on a RISING row: six counter steps (three presses, three releases).
        SubEdgeState R;
        R.Arrive(TriggerType::EdgeRising, 0, false); // baseline, even (lifted)
        R.NextPass();
        R.NextPass();
        CHECK(R.Arrive(TriggerType::EdgeRising, 6, false) == SubEdgeKind::Gap);
        CHECK_EQ(R.Budget, 3); // three presses, not six counter steps
        Sample(R, Run, 8);
        PrintRun("burst of 3 (EdgeRising, delta 6)", Run, 8);
        CHECK(std::strcmp(Run, "TTTF....") == 0);
    }

    // ---- A missed press: the counter advanced by two presses' worth, so BOTH are pulses. This is
    //      what makes a lost update recoverable instead of invisible.
    {
        SubEdgeState S;
        S.Arrive(TriggerType::EdgeRising, 1, false); // baseline: pressed
        S.NextPass();                                // the baseline's own pulse
        S.NextPass();                                // closed
        // Two presses happened, one push was lost: 1 -> 5 (press, lift, press, lift).
        CHECK(S.Arrive(TriggerType::EdgeRising, 5, false) == SubEdgeKind::Gap);
        CHECK_EQ(S.Missing, 3); // three counter steps never arrived
        CHECK_EQ(S.Budget, 2);  // but two of them are presses
        char Run[16];
        Sample(S, Run, 6);
        PrintRun("missed press + this one", Run, 6);
        CHECK(std::strcmp(Run, "TTF...") == 0);
    }

    // ---- Duplicate: a verified send that was retried (the ack was lost) is the same press and adds
    //      nothing - neither a second true nor a second log line.
    {
        SubEdgeState S;
        S.Arrive(TriggerType::EdgeAny, 4, false);
        CHECK(S.Arrive(TriggerType::EdgeAny, 5, false) == SubEdgeKind::Next);
        CHECK_EQ(S.Budget, 2); // the baseline push (1) and this edge (1)
        CHECK(S.Arrive(TriggerType::EdgeAny, 5, false) == SubEdgeKind::Duplicate);
        CHECK(S.Arrive(TriggerType::EdgeAny, 5, false) == SubEdgeKind::Duplicate);
        CHECK(!S.Report);
        CHECK_EQ(S.Budget, 2);
        CHECK_EQ(S.Last, 5);
    }

    // ---- An edge that arrives before the budget is exhausted EXTENDS the pulse by its own edges;
    //      nothing already owed is dropped or restarted.
    {
        SubEdgeState S;
        S.Arrive(TriggerType::EdgeAny, 10, false); // baseline: 1
        S.NextPass();
        S.NextPass();
        CHECK_EQ(S.Budget, 0);
        CHECK(S.Arrive(TriggerType::EdgeAny, 13, false) == SubEdgeKind::Gap); // 3 edges
        CHECK_EQ(S.Budget, 3);
        S.NextPass(); // one spent, two left
        CHECK_EQ(S.Budget, 2);
        CHECK(S.Arrive(TriggerType::EdgeAny, 15, false) == SubEdgeKind::Gap); // 2 more edges
        CHECK_EQ(S.Budget, 4); // 2 left + 2 new
        char Run[16];
        Sample(S, Run, 8);
        PrintRun("extended while running (2+2)", Run, 8);
        CHECK(std::strcmp(Run, "TTTTF...") == 0);

        // Extending with a single edge is +1, not a restart.
        SubEdgeState R;
        R.Arrive(TriggerType::EdgeAny, 0, false);
        R.NextPass();
        R.NextPass();
        R.Arrive(TriggerType::EdgeAny, 3, false); // 3 edges
        R.NextPass();                             // 2 left
        R.Arrive(TriggerType::EdgeAny, 4, false); // one more
        CHECK_EQ(R.Budget, 3);
        char Run2[16];
        Sample(R, Run2, 6);
        CHECK(std::strcmp(Run2, "TTTF..") == 0);
    }

    // ---- A hold that produces nothing: repeated updates with the same counter never re-arm.
    {
        SubEdgeState S;
        S.Arrive(TriggerType::EdgeRising, 7, false);
        S.NextPass();
        S.NextPass();
        for (int i = 0; i < 5; i++) {
            CHECK(S.Arrive(TriggerType::EdgeRising, 7, false) == SubEdgeKind::Duplicate);
            CHECK(!S.NextPass().Write); // nothing to write: the field is already false
        }
        CHECK_EQ(S.Budget, 0);
    }

    // ---- The walk over a wrap: the counter is u16, so 0xFFFF -> 0 is an ordinary step.
    {
        SubEdgeState S;
        S.Rebaseline(0xFFFE);
        CHECK(S.Arrive(TriggerType::EdgeAny, 0xFFFF, false) == SubEdgeKind::Next);
        CHECK(S.Arrive(TriggerType::EdgeAny, 0x0000, false) == SubEdgeKind::Next); // wrapped: +1
        CHECK(!S.Report);
        CHECK_EQ(S.Last, 0);
        CHECK_EQ(S.Budget, 2);
        // A gap that spans the wrap: 0x0001..0x0003 missing, four steps in all.
        CHECK(S.Arrive(TriggerType::EdgeAny, 0x0004, false) == SubEdgeKind::Gap);
        CHECK_EQ(S.Missing, 3);
        CHECK_EQ(S.Budget, 6);
    }

    // ---- Node reboot: the provider's counter restarts at zero. It must not read as a huge jump,
    //      and the pulses already owed are NOT thrown away.
    {
        // (a) the re-registration is answered first (the normal path: the node reappears, the row
        //     re-installs, the answer carries the restarted counter): a baseline, no gap, no log.
        SubEdgeState S;
        S.Arrive(TriggerType::EdgeAny, 57, false);
        CHECK(S.Arrive(TriggerType::EdgeAny, 60, false) == SubEdgeKind::Gap); // 58, 59 lost
        CHECK_EQ(S.Budget, 1 + 3);
        // The node reboots; its install is answered with counter 0.
        // The push is still a push - it exists because an edge happened - so it is one pulse on top
        // of the ones already owed; the reboot does not un-happen those.
        CHECK(S.Arrive(TriggerType::EdgeAny, 0, true) == SubEdgeKind::Baseline);
        CHECK_EQ(S.Last, 0);
        CHECK_EQ(S.Budget, 5);
        CHECK(!S.Report);
        CHECK(S.Arrive(TriggerType::EdgeAny, 1, false) == SubEdgeKind::Next);
        CHECK_EQ(S.Budget, 6);

        // (b) an edge update arrives BEFORE the install is answered (the provider still had the
        //     entry, its counter restarted): that is a resync, still not a jump of ~32k presses.
        SubEdgeState R;
        R.Arrive(TriggerType::EdgeAny, 57, false);
        R.NextPass();
        R.NextPass();
        CHECK(R.Arrive(TriggerType::EdgeAny, 1, false) == SubEdgeKind::Resync);
        CHECK(R.Report);        // the resync is logged (it means a node restarted unannounced)
        CHECK_EQ(R.Missing, 0); // ... but nothing is counted as missing
        CHECK_EQ(R.Budget, 0);  // and nothing is owed for it: the count restarted
        CHECK_EQ(R.Last, 1);    // the new counter is the baseline
        CHECK(R.Arrive(TriggerType::EdgeAny, 2, false) == SubEdgeKind::Next);
        CHECK_EQ(R.Budget, 1);
    }

    // ---- The budget is bounded: a huge gap is capped, and the cap is reported with its true size.
    {
        SubEdgeState S;
        S.Rebaseline(0);
        CHECK(S.Arrive(TriggerType::EdgeAny, 0x3000, false) == SubEdgeKind::Gap); // under the resync
        CHECK_EQ(S.Missing, 0x2FFF);
        CHECK_EQ(S.Budget, SUB_EDGE_PULSE_MAX); // saturating, not truncated to 0x3000 & 0xFF
        CHECK(S.Report);
        char Run[SUB_EDGE_PULSE_MAX + 2];
        Sample(S, Run, SUB_EDGE_PULSE_MAX + 1);
        unsigned Trues = 0;
        for (unsigned i = 0; i < SUB_EDGE_PULSE_MAX; i++) Trues += (Run[i] == 'T');
        CHECK_EQ(Trues, SUB_EDGE_PULSE_MAX);
        CHECK_EQ(Run[SUB_EDGE_PULSE_MAX], 'F'); // the pulse closes on the next pass
        CHECK_EQ(S.Budget, 0);

        // The resync boundary itself: half the ring away is a restarted provider, not a gap.
        SubEdgeState R;
        R.Rebaseline(0);
        CHECK(R.Arrive(TriggerType::EdgeAny, SUB_EDGE_RESYNC_DELTA, false) == SubEdgeKind::Resync);
        R.Rebaseline(0);
        CHECK(R.Arrive(TriggerType::EdgeAny, SUB_EDGE_RESYNC_DELTA - 1, false) == SubEdgeKind::Gap);
    }

    // ---- A target that cannot take a pulse: the rest of the budget is given up, exactly once, with
    //      its size - and the row then stops writing instead of retrying for ever.
    {
        SubEdgeState S;
        S.Arrive(TriggerType::EdgeAny, 1, false);
        S.Arrive(TriggerType::EdgeAny, 5, false); // 2, 3, 4 never arrived
        CHECK_EQ(S.Budget, 5);
        const uint8_t Lost = S.GiveUp();
        CHECK_EQ(Lost, 5);
        CHECK_EQ(S.Budget, 0);
        CHECK_EQ(S.GiveUp(), 0); // a second give-up reports nothing
        CHECK(!S.NextPass().Write); // nothing more is written to a target that refused
    }

    // ---- Reset rules: an install (and a recall, which allocates a fresh row) starts the baseline
    //      over, so the next counter is a baseline instead of a gap against the previous row's, and
    //      the target is cleared once rather than trusted.
    {
        SubEdgeState S;
        S.Arrive(TriggerType::EdgeAny, 10, false);
        S.Arrive(TriggerType::EdgeAny, 13, false);
        CHECK_EQ(S.Budget, 4);
        S.NextPass();
        S.ResetBaseline();
        CHECK(!S.HaveBaseline);
        CHECK_EQ(S.Budget, 0);
        CHECK(S.High); // a fresh row re-clears the target
        CHECK(S.Arrive(TriggerType::EdgeAny, 11, false) == SubEdgeKind::Baseline);
        CHECK_EQ(S.Budget, 1); // the push that re-started the row is still one press
    }

    // ---- The lines the device prints. SubFormatEdgeGap / SubFormatEdgeResync / SubFormatEdgeDrop
    //      are what SubscriptionsRequester.h hands to DeviceLog, so their exact text is pinned here,
    //      along with the guarantee that the longest line still fits DeviceLog's 128-byte buffer.
    {
        char Line[SUB_EDGE_TEXT_MAX];
        unsigned Len = SubFormatEdgeGap(Line, sizeof(Line), 0x1003, 0x0204, 7, 9, 2);
        CHECK_EQ(Len, std::strlen(Line));
        CHECK(std::strcmp(Line,
                          "TRID 1003 prov 0204: gap of 2 edges (counter 7 -> 9), one pulse per pass") == 0);

        // Worst case (full-width TRID/address/counters, 5-digit count) must not be truncated.
        char Worst[SUB_EDGE_TEXT_MAX];
        Len = SubFormatEdgeGap(Worst, sizeof(Worst), 0xFFFF, 0xFFFF, 65535, 65535, 32767);
        CHECK(Len < SUB_EDGE_TEXT_MAX);
        CHECK_EQ(Len, std::strlen(Worst));
        CHECK(Len < 128); // DeviceLog's Buffer

        Len = SubFormatEdgeResync(Line, sizeof(Line), 0x1003, 0x0204, 57, 1);
        CHECK_EQ(Len, std::strlen(Line));
        CHECK(std::strcmp(Line,
                          "TRID 1003 prov 0204: counter went back (57 -> 1), "
                          "provider restarted, baseline resynced") == 0);
        Len = SubFormatEdgeResync(Worst, sizeof(Worst), 0xFFFF, 0xFFFF, 65535, 0);
        CHECK(Len < SUB_EDGE_TEXT_MAX);
        CHECK_EQ(Len, std::strlen(Worst));

        // The give-up line: the target would not take the pulse, so say how many edges are lost.
        Len = SubFormatEdgeDrop(Line, sizeof(Line), 0x1003, 0x0204, 3);
        CHECK_EQ(Len, std::strlen(Line));
        CHECK(std::strcmp(Line, "TRID 1003 prov 0204: target refused the pulse, 3 edges dropped") == 0);
        Len = SubFormatEdgeDrop(Worst, sizeof(Worst), 0xFFFF, 0xFFFF, 255);
        CHECK(Len < SUB_EDGE_TEXT_MAX);
        CHECK_EQ(Len, std::strlen(Worst));
        CHECK(Len < 128); // DeviceLog's Buffer

        // One tag shared with the rest of the service's lines.
        CHECK(std::strcmp(SUB_EDGE_TAG, "SUB") == 0);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
