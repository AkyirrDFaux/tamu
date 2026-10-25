// Host-side test for the edge counter an edge-triggered value update carries
// (Core/Services/SubscriptionsEdge.h).
//
// The defect this pins: an edge-triggered update is a *press*, and the wire used to carry the
// value alone - a Bool. A press that arrived and a press that was lost therefore looked identical
// at the requester: the target already held the value, so a lost update changed nothing and said
// nothing. The provider has always counted its edges (ProviderEntry::Hash, Docs "Counter: for Bool
// edges only") but the counter never crossed the wire. It does now: the value update for an edge
// trigger is the counter (u16 little-endian) ahead of the value bytes.
//
// What is under test is the real state machine the core runs, not a copy of it: RequesterEntry
// embeds SubEdgeState and the requester only calls its methods (SubscriptionsEdge.h), so this file
// drives Arrive/Retain/OnReplayed/GiveUp exactly as Core/Services/SubscriptionsRequester.h does.
// The classification is:
//   Baseline  - the first counter since the row was installed (or since the provider answered a
//               re-registration): the new zero point, never a gap
//   Duplicate - the same counter again (a verified send retried after a lost ack): ignored
//   Next      - counter + 1, the normal case, and also what a wrap looks like (modular)
//   Gap       - counter + n (n >= 2): n-1 presses were never seen, replayed one per pass
//   Resync    - the counter went backwards: a restarted provider (a node's table is RAM-only),
//               counted as a baseline break, never as a jump of ~32k presses
//
// The two ends' shared encoding lives in the same header (SubEdgeCounterStore/Load), so this file
// also pins the byte order - if the two ends ever disagree about it, every update is a "gap".
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

int main() {
    // ---- The wire encoding: u16 little-endian, ahead of the value bytes.
    {
        CHECK_EQ(SUB_EDGE_COUNTER_SIZE, 2);
        uint8_t Wire[8] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
        SubEdgeCounterStore(Wire, 0x1234);
        CHECK_EQ(Wire[0], 0x34); // low byte first
        CHECK_EQ(Wire[1], 0x12);
        CHECK_EQ(Wire[2], 0xEE); // nothing else touched
        CHECK_EQ(SubEdgeCounterLoad(Wire), 0x1234);
        // The provider's counter is 32-bit (it increments a uint32): the wire carries its low 16
        // bits and wraps there, so 0x10001 is the same wire value as 0x0001.
        SubEdgeCounterStore(Wire, 0x10001);
        CHECK_EQ(SubEdgeCounterLoad(Wire), 0x0001);
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

    // ---- Normal sequence: the first counter is the baseline, then +1 each time.
    {
        SubEdgeState S;
        CHECK(!S.HaveBaseline);
        CHECK(S.Arrive(7, false) == SubEdgeKind::Baseline);
        CHECK(S.HaveBaseline);
        CHECK(!S.Report); // a baseline is not a gap: nothing to log
        CHECK_EQ(S.Pending, 0);
        CHECK_EQ(S.Last, 7);
        CHECK(S.Arrive(8, false) == SubEdgeKind::Next);
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
        CHECK_EQ(S.Pending, 0);   // ... and nothing is replayed
        CHECK_EQ(S.Last, 5);
        CHECK(S.Arrive(6, false) == SubEdgeKind::Next);
    }

    // ---- Gap of 2: two presses are missing, one is delivered by the update itself.
    {
        SubEdgeState S;
        S.Arrive(3, false);
        CHECK(S.Arrive(6, false) == SubEdgeKind::Gap); // 4 and 5 never arrived
        CHECK(S.Report);                               // the episode is opened: log it once
        CHECK_EQ(S.Missing, 2);                        // delta - 1
        CHECK_EQ(S.Pending, 2);                        // two replays owed
        CHECK_EQ(S.Last, 6);
        S.Report = false;
        // A second gap while the episode is open: accounted, but not logged again.
        CHECK(S.Arrive(9, false) == SubEdgeKind::Gap); // 7 and 8 missing
        CHECK(!S.Report);
        CHECK_EQ(S.Missing, 2);
        CHECK_EQ(S.Pending, 4);
        // One replay per pass, and the episode closes when the debt is paid.
        S.OnReplayed();
        CHECK_EQ(S.Pending, 3);
        S.OnReplayed();
        S.OnReplayed();
        CHECK_EQ(S.Pending, 1);
        S.OnReplayed();
        CHECK_EQ(S.Pending, 0);
        CHECK(!S.Episode);
        // Paid up: the next gap is a new episode and is logged again.
        CHECK(S.Arrive(12, false) == SubEdgeKind::Gap);
        CHECK(S.Report);
        CHECK_EQ(S.Pending, 2);
    }

    // ---- The replay payload: the value the gap's update carried, retained for the replays.
    {
        SubEdgeState S;
        S.Arrive(1, false);
        const uint8_t One = 1;
        CHECK(S.Retain(&One, 1));
        CHECK(S.CanReplay());
        CHECK_EQ(S.ValueLen, 1);
        CHECK_EQ(S.Value[0], 1);
        // A value that cannot be retained (0-length, or wider than the buffer) cannot be replayed.
        SubEdgeState W;
        CHECK(!W.Retain(&One, 0));
        CHECK(!W.CanReplay());
        const uint8_t Wide[SUB_EDGE_REPLAY_VALUE_MAX + 1] = {1, 2, 3, 4, 5};
        CHECK(!W.Retain(Wide, sizeof(Wide)));
        CHECK(!W.CanReplay());
        uint8_t Fits[SUB_EDGE_REPLAY_VALUE_MAX] = {9, 8, 7, 6};
        CHECK(W.Retain(Fits, SUB_EDGE_REPLAY_VALUE_MAX));
        CHECK(W.CanReplay());
        CHECK_EQ(W.Value[0], 9);
        CHECK_EQ(W.Value[SUB_EDGE_REPLAY_VALUE_MAX - 1], 6);
    }

    // ---- Wrap: the counter is u16, so 0xFFFF -> 0 is an ordinary step, not a gap.
    {
        SubEdgeState S;
        S.Rebaseline(0xFFFE);
        CHECK(S.Arrive(0xFFFF, false) == SubEdgeKind::Next);
        CHECK(S.Arrive(0x0000, false) == SubEdgeKind::Next); // wrapped: still +1
        CHECK(!S.Report);
        CHECK_EQ(S.Pending, 0);
        CHECK_EQ(S.Last, 0);
        CHECK(S.Arrive(0x0001, false) == SubEdgeKind::Next);
        // A gap that spans the wrap is counted the same way: 0x0002..0x0004 missing.
        CHECK(S.Arrive(0x0005, false) == SubEdgeKind::Gap);
        CHECK_EQ(S.Missing, 3);
        CHECK_EQ(S.Pending, 3);
    }

    // ---- Node reboot: the provider's counter restarts at zero. It must not read as a huge jump.
    {
        // (a) the re-registration is answered first (the normal path: the node reappears, the row
        //     re-installs, the answer carries the restarted counter): a baseline, no gap, no log.
        SubEdgeState S;
        S.Arrive(57, false);
        CHECK(S.Arrive(58, false) == SubEdgeKind::Next);
        CHECK(S.Arrive(60, false) == SubEdgeKind::Gap); // 59 was lost
        CHECK_EQ(S.Pending, 1);
        // The node reboots; its install is answered with counter 0.
        CHECK(S.Arrive(0, true) == SubEdgeKind::Baseline);
        CHECK_EQ(S.Last, 0);
        CHECK_EQ(S.Pending, 0);   // the replays owed for the old counter are meaningless now
        CHECK(!S.Report);
        CHECK(!S.Episode);
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

    // ---- Lost packet, then recovery: 6 is lost, 7 arrives, the missed press is replayed once.
    {
        SubEdgeState S;
        S.Arrive(5, false);
        const uint8_t One = 1;
        S.Retain(&One, 1);
        CHECK(S.Arrive(7, false) == SubEdgeKind::Gap); // 6 was lost
        CHECK(S.Report);
        CHECK_EQ(S.Missing, 1);
        CHECK_EQ(S.Pending, 1);
        CHECK(S.CanReplay());
        // The value update that revealed the loss is applied first (the caller does that), then one
        // pass replays the missing press ...
        S.OnReplayed();
        CHECK_EQ(S.Pending, 0);
        // ... and the subscription is back to normal: the next press is a plain +1.
        CHECK(S.Arrive(8, false) == SubEdgeKind::Next);
        CHECK_EQ(S.Pending, 0);
    }

    // ---- A target that cannot take a replay: the debt is given up, exactly once, with its size.
    {
        SubEdgeState S;
        S.Arrive(1, false);
        S.Arrive(5, false);
        CHECK_EQ(S.Pending, 3);
        const uint8_t Lost = S.GiveUp();
        CHECK_EQ(Lost, 3);
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
        CHECK_EQ(S.Pending, 2);
        S.ResetBaseline();
        CHECK(!S.HaveBaseline);
        CHECK_EQ(S.Pending, 0);
        CHECK(!S.Episode);
        CHECK(S.Arrive(11, false) == SubEdgeKind::Baseline);
        CHECK_EQ(S.Pending, 0);
    }

    // ---- The saturating debt: a gap larger than the replay budget is capped, never wrapped.
    {
        SubEdgeState S;
        S.Rebaseline(0);
        CHECK(S.Arrive(0x3000, false) == SubEdgeKind::Gap); // still under the resync boundary
        CHECK_EQ(S.Missing, 0x2FFF);
        CHECK_EQ(S.Pending, SUB_EDGE_PENDING_MAX); // saturating, not truncated to 0x2FFF & 0xFF
        CHECK(S.Report);
        // The resync boundary itself: half the ring away is a restarted provider, not a gap.
        SubEdgeState R;
        R.Rebaseline(0);
        CHECK(R.Arrive(SUB_EDGE_RESYNC_DELTA, false) == SubEdgeKind::Resync);
        R.Rebaseline(0);
        CHECK(R.Arrive(SUB_EDGE_RESYNC_DELTA - 1, false) == SubEdgeKind::Gap);
    }

    // ---- The lines the device prints. SubFormatEdgeGap / SubFormatEdgeResync are what
    //      SubscriptionsRequester.h hands to DeviceLog, so their exact text is pinned here, along
    //      with the guarantee that the longest line still fits DeviceLog's 128-byte buffer.
    {
        char Line[SUB_EDGE_TEXT_MAX];
        unsigned Len = SubFormatEdgeGap(Line, sizeof(Line), 0x1003, 0x0204, 7, 9, 2, true);
        CHECK_EQ(Len, std::strlen(Line));
        CHECK(std::strcmp(Line,
                          "TRID 1003 prov 0204: gap of 2 edges (counter 7 -> 9), replaying") == 0);

        // No replay possible: the same loss, said as a loss.
        Len = SubFormatEdgeGap(Line, sizeof(Line), 0x1000, 0x0204, 65530, 1, 6, false);
        CHECK_EQ(Len, std::strlen(Line));
        CHECK(std::strcmp(Line,
                          "TRID 1000 prov 0204: gap of 6 edges (counter 65530 -> 1), "
                          "target cannot take a replay, dropped") == 0);

        // Worst case (full-width TRID/address/counters, 5-digit count) must not be truncated.
        char Worst[SUB_EDGE_TEXT_MAX];
        Len = SubFormatEdgeGap(Worst, sizeof(Worst), 0xFFFF, 0xFFFF, 65535, 65535, 32767, false);
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

        // The give-up line: the replay could not be delivered, so say how many edges are lost.
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
