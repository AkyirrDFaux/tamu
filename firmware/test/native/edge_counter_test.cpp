// Host-side test for the edge counter that IS an edge-triggered value update's payload
// (Core/Services/SubscriptionsEdge.h).
//
// The defect this pins: an edge-triggered update is a *press*, and the wire used to carry the value
// alone - a Bool. A press that arrived and a press that was lost therefore looked identical at the
// requester: the target already held the value, so a lost update changed nothing and said nothing.
// The rule now: the provider's edge counter is the update's WHOLE payload (u16 little-endian, two
// bytes, nothing appended) and its PARITY carries the level - odd = true/pressed, even =
// false/lifted. A counter that advanced by n means n edges, and the requester walks them one level
// per pass, oldest first, ending on the newest counter so the target is left on the true level.
//
// What is under test is the real state machine the core runs, not a copy of it: RequesterEntry
// embeds SubEdgeState and the requester only calls its methods (SubscriptionsEdge.h), so this file
// drives Arrive/ReplayLevel/OnReplayed/GiveUp exactly as Core/Services/SubscriptionsRequester.h
// does, and SubEdgeClassify exactly as Core/Services/SubscriptionsProvider.h does. The
// classification is:
//   Baseline  - the first counter since the row was installed (or since the provider answered a
//               re-registration): the new zero point, never a gap
//   Duplicate - the same counter again (a verified send retried after a lost ack): ignored
//   Next      - counter + 1, the normal case, and also what a wrap looks like (modular)
//   Gap       - counter + n (n >= 2): n-1 presses were never seen, walked one level per pass
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

// Drains the replay walk, collecting the level of every pass in order.
static unsigned Drain(SubEdgeState &State, uint8_t *Out, unsigned Max) {
    unsigned n = 0;
    while (State.Pending && n < Max) {
        Out[n++] = State.ReplayLevel();
        State.OnReplayed();
    }
    return n;
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

    // ---- Parity IS the level: odd = true/pressed, even = false/lifted.
    {
        CHECK(!SubEdgeLevel(0));
        CHECK(SubEdgeLevel(1));
        CHECK(!SubEdgeLevel(2));
        CHECK(SubEdgeLevel(3));
        CHECK(!SubEdgeLevel(0xFFFE));
        CHECK(SubEdgeLevel(0xFFFF));
        CHECK_EQ(SubEdgeLevelByte(0), 0);
        CHECK_EQ(SubEdgeLevelByte(1), 1);
        CHECK_EQ(SubEdgeLevelByte(0x1234), 0);
        CHECK_EQ(SubEdgeLevelByte(0x1235), 1);
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

    // ---- Normal sequence: the first counter is the baseline, then +1 each time, and the level
    //      alternates with it.
    {
        SubEdgeState S;
        CHECK(!S.HaveBaseline);
        CHECK(S.Arrive(7, false) == SubEdgeKind::Baseline);
        CHECK(S.HaveBaseline);
        CHECK(!S.Report); // a baseline is not a gap: nothing to log
        CHECK_EQ(S.Pending, 0);
        CHECK_EQ(S.Last, 7);
        CHECK_EQ(SubEdgeLevelByte(7), 1); // baseline counter odd: the source is pressed
        CHECK(S.Arrive(8, false) == SubEdgeKind::Next);
        CHECK_EQ(SubEdgeLevelByte(8), 0);
        CHECK(S.Arrive(9, false) == SubEdgeKind::Next);
        CHECK(S.Arrive(10, false) == SubEdgeKind::Next);
        CHECK(!S.Report);
        CHECK_EQ(S.Pending, 0);
        CHECK_EQ(S.Last, 10);
    }

    // ---- Duplicate: a verified send that was retried (the ack was lost) is the same press.
    {
        SubEdgeState S;
        S.Arrive(4, false);
        CHECK(S.Arrive(5, false) == SubEdgeKind::Next);
        CHECK(S.Arrive(5, false) == SubEdgeKind::Duplicate);
        CHECK(S.Arrive(5, false) == SubEdgeKind::Duplicate);
        CHECK(!S.Report);         // nothing is logged for a duplicate ...
        CHECK_EQ(S.Pending, 0);   // ... and no level is owed
        CHECK_EQ(S.Last, 5);
        CHECK(S.Arrive(6, false) == SubEdgeKind::Next);
    }

    // ---- Gap of 2: two edges were never seen. The walk owes the whole run - the two missed levels
    //      AND the arriving one, oldest first - and ends on the newest counter, so the target is
    //      left on the level that counter carries.
    {
        SubEdgeState S;
        S.Arrive(3, false);
        CHECK(S.Arrive(6, false) == SubEdgeKind::Gap); // 4 and 5 never arrived
        CHECK(S.Report);                               // the episode is opened: log it once
        CHECK_EQ(S.Missing, 2);                        // delta - 1, what the log reports
        CHECK_EQ(S.Pending, 3);                        // 4, 5 and 6 are owed as levels
        CHECK_EQ(S.ReplayNext, 4);                     // the oldest edge that was never seen
        CHECK_EQ(S.Last, 6);

        uint8_t Levels[8] = {};
        CHECK_EQ(Drain(S, Levels, 8), 3);
        CHECK_EQ(Levels[0], 0); // counter 4: even -> lifted
        CHECK_EQ(Levels[1], 1); // counter 5: odd  -> pressed
        CHECK_EQ(Levels[2], 0); // counter 6: even -> lifted, the level the update carried
        CHECK_EQ(S.Pending, 0);
        CHECK(!S.Episode);
        CHECK_EQ(S.Last, 6);
        // Paid up: the walk is gone, and the next edge lands on its own.
        CHECK(S.Arrive(7, false) == SubEdgeKind::Next);
        CHECK_EQ(S.Pending, 0);
    }

    // ---- A second gap while the episode is open: accounted, not logged again, and the walk still
    //      ends on the newest counter.
    {
        SubEdgeState S;
        S.Arrive(3, false);
        CHECK(S.Arrive(6, false) == SubEdgeKind::Gap);
        S.Report = false;
        CHECK(S.Arrive(9, false) == SubEdgeKind::Gap); // 7 and 8 missing
        CHECK(!S.Report);
        CHECK_EQ(S.Missing, 2);
        CHECK_EQ(S.Pending, 6);    // 4,5,6 from the first gap and 7,8,9 from this one
        CHECK_EQ(S.ReplayNext, 4); // continues where it stopped: oldest first
        uint8_t Levels[8] = {};
        CHECK_EQ(Drain(S, Levels, 8), 6);
        CHECK_EQ(Levels[5], 1); // counter 9: odd -> pressed, the newest level stands
        CHECK(!S.Episode);
        // A press that arrives while the walk is still paying is appended to the walk, not written
        // out of order: the last level must still be the newest counter's parity.
        SubEdgeState R;
        R.Arrive(3, false);
        R.Arrive(6, false);
        CHECK_EQ(R.Pending, 3);
        CHECK(R.Arrive(7, false) == SubEdgeKind::Next);
        CHECK_EQ(R.Pending, 4);
        uint8_t Tail[8] = {};
        CHECK_EQ(Drain(R, Tail, 8), 4);
        CHECK_EQ(Tail[3], SubEdgeLevelByte(7)); // ends on 7, not on 6
        CHECK_EQ(Tail[0], SubEdgeLevelByte(4));
        CHECK(!R.Episode);
        // Paid up: the next gap is a new episode and is logged again.
        CHECK(R.Arrive(10, false) == SubEdgeKind::Gap);
        CHECK(R.Report);
    }

    // ---- The walk over a wrap: the counter is u16, so 0xFFFF -> 0 is an ordinary step, not a gap.
    {
        SubEdgeState S;
        S.Rebaseline(0xFFFE);
        CHECK(S.Arrive(0xFFFF, false) == SubEdgeKind::Next);
        CHECK(S.Arrive(0x0000, false) == SubEdgeKind::Next); // wrapped: still +1
        CHECK(!S.Report);
        CHECK_EQ(S.Pending, 0);
        CHECK_EQ(S.Last, 0);
        CHECK(S.Arrive(0x0001, false) == SubEdgeKind::Next);
        // A gap that spans the wrap: 0x0002..0x0004 missing, and the walk rolls over with it.
        CHECK(S.Arrive(0x0005, false) == SubEdgeKind::Gap);
        CHECK_EQ(S.Missing, 3);
        CHECK_EQ(S.Pending, 4);
        CHECK_EQ(S.ReplayNext, 0x0002);
        uint8_t Levels[8] = {};
        CHECK_EQ(Drain(S, Levels, 8), 4);
        CHECK_EQ(Levels[0], SubEdgeLevelByte(0x0002));
        CHECK_EQ(Levels[3], 1); // counter 5: odd -> pressed
    }

    // ---- A gap that wraps past 0xFFFF: the walk is modular, so it walks 0xFFFE, 0xFFFF, 0x0000 ...
    {
        SubEdgeState S;
        S.Rebaseline(0xFFFD);
        CHECK(S.Arrive(0x0001, false) == SubEdgeKind::Gap); // 0xFFFE, 0xFFFF, 0x0000 missing
        CHECK_EQ(S.Missing, 3);
        CHECK_EQ(S.ReplayNext, 0xFFFE);
        uint8_t Levels[8] = {};
        CHECK_EQ(Drain(S, Levels, 8), 4);
        CHECK_EQ(Levels[0], 0); // 0xFFFE even
        CHECK_EQ(Levels[1], 1); // 0xFFFF odd
        CHECK_EQ(Levels[2], 0); // 0x0000 even
        CHECK_EQ(Levels[3], 1); // 0x0001 odd, the newest
    }

    // ---- Node reboot: the provider's counter restarts at zero. It must not read as a huge jump.
    {
        // (a) the re-registration is answered first (the normal path: the node reappears, the row
        //     re-installs, the answer carries the restarted counter): a baseline, no gap, no log.
        SubEdgeState S;
        S.Arrive(57, false);
        CHECK(S.Arrive(58, false) == SubEdgeKind::Next);
        CHECK(S.Arrive(60, false) == SubEdgeKind::Gap); // 59 was lost
        CHECK_EQ(S.Pending, 2);
        // The node reboots; its install is answered with counter 0.
        CHECK(S.Arrive(0, true) == SubEdgeKind::Baseline);
        CHECK_EQ(S.Last, 0);
        CHECK_EQ(S.Pending, 0);   // the levels owed for the old counter are meaningless now
        CHECK(!S.Report);
        CHECK(!S.Episode);
        CHECK_EQ(SubEdgeLevelByte(0), 0); // a restarted counter is a lifted switch
        CHECK(S.Arrive(1, false) == SubEdgeKind::Next);

        // (b) an edge update arrives BEFORE the install is answered (the provider still had the
        //     entry, its counter restarted): that is a resync, still not a jump of ~32k presses.
        SubEdgeState R;
        R.Arrive(57, false);
        CHECK(R.Arrive(1, false) == SubEdgeKind::Resync);
        CHECK(R.Report);          // the resync is logged (it means a node restarted unannounced)
        CHECK_EQ(R.Missing, 0);   // ... but nothing is counted as missing
        CHECK_EQ(R.Pending, 0);
        CHECK_EQ(R.Last, 1);      // the new counter is the baseline
        CHECK(R.Arrive(2, false) == SubEdgeKind::Next);
    }

    // ---- The debt is bounded: a huge gap is capped, and the cap still ends the walk on the newest
    //      counter (dropping from the FRONT, because a walk that stopped short would leave the
    //      target on the wrong side of the level).
    {
        SubEdgeState S;
        S.Rebaseline(0);
        CHECK(S.Arrive(0x3000, false) == SubEdgeKind::Gap); // still under the resync boundary
        CHECK_EQ(S.Missing, 0x2FFF);
        CHECK_EQ(S.Pending, SUB_EDGE_PENDING_MAX); // saturating, not truncated to 0x2FFF & 0xFF
        CHECK_EQ(S.ReplayNext, 0x3000 - (SUB_EDGE_PENDING_MAX - 1));
        CHECK(S.Report);
        uint8_t Levels[SUB_EDGE_PENDING_MAX];
        CHECK_EQ(Drain(S, Levels, SUB_EDGE_PENDING_MAX), SUB_EDGE_PENDING_MAX);
        CHECK_EQ(Levels[SUB_EDGE_PENDING_MAX - 1], SubEdgeLevelByte(0x3000)); // ends on the newest
        CHECK_EQ(S.Pending, 0);
        CHECK(!S.Episode);

        // The resync boundary itself: half the ring away is a restarted provider, not a gap.
        SubEdgeState R;
        R.Rebaseline(0);
        CHECK(R.Arrive(SUB_EDGE_RESYNC_DELTA, false) == SubEdgeKind::Resync);
        R.Rebaseline(0);
        CHECK(R.Arrive(SUB_EDGE_RESYNC_DELTA - 1, false) == SubEdgeKind::Gap);
    }

    // ---- A target that cannot take a level: the debt is given up, exactly once, with its size.
    {
        SubEdgeState S;
        S.Arrive(1, false);
        S.Arrive(5, false); // 2,3,4 missing: four levels owed (2,3,4,5)
        CHECK_EQ(S.Pending, 4);
        const uint8_t Lost = S.GiveUp();
        CHECK_EQ(Lost, 4);
        CHECK_EQ(S.Pending, 0);
        CHECK(!S.Episode);
        CHECK_EQ(S.GiveUp(), 0); // a second give-up reports nothing
    }

    // ---- Reset rules: an install (and a recall, which allocates a fresh row) starts the baseline
    //      over, so the next counter is a baseline instead of a gap against the previous row's.
    {
        SubEdgeState S;
        S.Arrive(10, false);
        S.Arrive(13, false);
        CHECK_EQ(S.Pending, 3);
        S.ResetBaseline();
        CHECK(!S.HaveBaseline);
        CHECK_EQ(S.Pending, 0);
        CHECK(!S.Episode);
        CHECK(S.Arrive(11, false) == SubEdgeKind::Baseline);
        CHECK_EQ(S.Pending, 0);
    }

    // ---- The lines the device prints. SubFormatEdgeGap / SubFormatEdgeResync / SubFormatEdgeDrop
    //      are what SubscriptionsRequester.h hands to DeviceLog, so their exact text is pinned here,
    //      along with the guarantee that the longest line still fits DeviceLog's 128-byte buffer.
    {
        char Line[SUB_EDGE_TEXT_MAX];
        unsigned Len = SubFormatEdgeGap(Line, sizeof(Line), 0x1003, 0x0204, 7, 9, 2);
        CHECK_EQ(Len, std::strlen(Line));
        CHECK(std::strcmp(Line,
                          "TRID 1003 prov 0204: gap of 2 edges (counter 7 -> 9), one level per pass") == 0);

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

        // The give-up line: the walk could not be delivered, so say how many edges are lost.
        Len = SubFormatEdgeDrop(Line, sizeof(Line), 0x1003, 0x0204, 12, 3);
        CHECK_EQ(Len, std::strlen(Line));
        CHECK(std::strcmp(Line, "TRID 1003 prov 0204: replay stopped at counter 12, 3 edges dropped") == 0);
        Len = SubFormatEdgeDrop(Worst, sizeof(Worst), 0xFFFF, 0xFFFF, 65535, 255);
        CHECK(Len < SUB_EDGE_TEXT_MAX);
        CHECK_EQ(Len, std::strlen(Worst));
        CHECK(Len < 128); // DeviceLog's Buffer

        // One tag shared with the rest of the service's lines.
        CHECK(std::strcmp(SUB_EDGE_TAG, "SUB") == 0);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
