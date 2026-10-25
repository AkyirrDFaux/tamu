// Host-side test for the renewal contract (owner ruling, 2026-10-25):
//
//   "a renewal is just a standard 0401 command; if the provider already does that subscription it
//    just extends the timeout, and responds to that command as usual. No hash or value reset. The
//    requester extends the timeout on receiving a successful reply."
//   "The renewal should only extend the timeouts if the subscription was already running...
//    nothing else special should happen."
//
// Three rules, and where each one is pinned:
//
//  1. A renewal of a RUNNING subscription extends only the timeout. The parts that live in host-clean
//     code are driven here: the answer decision (SubEdgeAnswer -> Renew for a served row, so no
//     re-baseline), the lease (an answer restarts Unanswered and puts the row back on the keepalive),
//     and the counter (a push after a renewal is Next, not Baseline - the provider no longer restarts
//     its counter, so a press must be compared against the row's last one).
//  2. A renewal of a NON-RUNNING subscription changes nothing. The same decision answers Install for
//     a never-served row - the ONE place a reply may baseline the counter and start the row from the
//     source's current value (Docs CID 1) - while an existing provider entry is only given the new
//     timeout (ProviderApply's `Reset` is false on the 0401 path, true only for the explicit 0421),
//     and that decision is host-clean code (SubscriptionsProviderReset.h) driven here: an identical
//     re-install keeps every piece of trigger state, while a new entry, a changed source or a
//     changed trigger resets it. ProviderApply's and the requester's target write pull the
//     packet/register/device layer and do not link on the host, so run.sh asserts those call sites
//     structurally.
//  3. A FAILED renewal leaves the row intact and a later renewal succeeds. The lease has no removal
//     path at all (run.sh pins that the only requester removal is RequesterSet's cancel branch), so
//     six unanswered renewals leave the row served-or-not exactly as it was, retrying on the
//     keepalive cadence, and the next answer restores the steady state.
//
// The cadence has to beat the provider's lease with margin: two keepalives fit inside SUB_TIMEOUT_MS,
// so one lost renewal cannot let the provider's entry lapse before the next one lands.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdint>
#include <cstdio>

#include "Core/Services/SubscriptionsEdge.h"
#include "Core/Services/SubscriptionsLease.h"
#include "Core/Services/SubscriptionsProviderReset.h"

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
            std::printf("FAIL %s:%d: %s == %s (got %lld, want %lld)\n", __FILE__,    \
                        __LINE__, #got, #want, g, w);                                \
        }                                                                            \
    } while (0)

// One requester row's transient state, as the device holds it after the 28 B wire prefix.
struct Row {
    SubRequesterLease Lease;
    SubEdgeState Edge;
    uint32_t Timeout = 0;
};

// The requester's send (RegisterRequesterProvider): it never extends the row's own deadline.
static void SendRenewal(Row &R, uint32_t Now) { R.Lease.OnSend(Now); }

// The provider's answer to that 0401. The row's reaction is decided by SubEdgeAnswer, which is what
// the device calls; the statement sequence around it is guarded structurally by run.sh (the Renew
// branch must not touch the edge state or the target, and must extend the deadline).
static void Answer(Row &R, uint32_t Now) { R.Lease.OnAnswer(Now); }

int main() {
    // ---- 1. A renewal of a running subscription extends only the timeout --------------------
    {
        Row R;
        // The row was installed and answered: it is running.
        R.Lease.OnSend(1000);
        Answer(R, 1100);
        R.Timeout = SubTimeoutFrom(1100);
        R.Edge.Rebaseline(14); // the counter the provider reported at install
        CHECK(R.Lease.Served());
        CHECK_EQ(SubEdgeAnswer(R.Lease.Served()) == SubEdgeAnswerKind::Renew, 1);

        // The renewal is due on the keepalive cadence measured from the last SEND (the answer does
        // not move that clock: only a send does).
        const uint32_t RenewAt = 1000 + SUB_KEEPALIVE_MS;
        CHECK(R.Lease.Due(RenewAt));       // the renewal is due on the keepalive cadence
        CHECK(!R.Lease.Due(RenewAt - 1));  // ...and not before it

        // The send itself changes nothing but the attempt count.
        const uint32_t LeaseBefore = R.Lease.LastValueMs;
        const uint16_t CounterBefore = R.Edge.Last;
        const bool BaselineBefore = R.Edge.HaveBaseline;
        SendRenewal(R, RenewAt);
        CHECK_EQ(R.Lease.LastValueMs, LeaseBefore); // no optimistic extension on the send
        CHECK_EQ(R.Lease.Unanswered, 1);
        CHECK_EQ(R.Edge.Last, CounterBefore);
        CHECK_EQ(R.Edge.HaveBaseline, BaselineBefore);

        // The answer extends the deadline and puts the row back on the keepalive - nothing else.
        Answer(R, RenewAt + 30);
        R.Timeout = SubTimeoutFrom(RenewAt + 30);
        CHECK_EQ(R.Lease.Unanswered, 0);
        CHECK_EQ(R.Lease.Interval(), SUB_KEEPALIVE_MS);
        CHECK_EQ(R.Timeout, RenewAt + 30 + SUB_TIMEOUT_MS);

        // A renewal does not re-baseline the counter: the next press is compared, so the press that
        // happened ABOVE the old baseline is still accounted (a press counted across a renewal is
        // Next, not the new zero point).
        CHECK_EQ((int)R.Edge.Arrive(15, SubEdgeAnswer(R.Lease.Served()) == SubEdgeAnswerKind::Install),
                 (int)SubEdgeKind::Next);
        CHECK_EQ(R.Edge.Last, 15);
        CHECK_EQ(R.Edge.Pending, 0); // delta 1: nothing owed, the level is written live
        // ...and the release is counted by the provider but not sent, so the next press is a gap
        // that the walk delivers one level per pass - still ending on the newest counter.
        CHECK_EQ((int)R.Edge.Arrive(17, false), (int)SubEdgeKind::Gap);
        CHECK_EQ(R.Edge.Missing, 1);
        CHECK_EQ(R.Edge.Pending, 2);
        CHECK_EQ(R.Edge.ReplayLevel(), 0); // counter 16: lifted
        R.Edge.OnReplayed();
        CHECK_EQ(R.Edge.ReplayLevel(), 1); // counter 17: pressed, the level this update carries
        R.Edge.OnReplayed();
        CHECK_EQ(R.Edge.Pending, 0);
        CHECK_EQ(R.Edge.Last, 17);
    }

    // ---- 2. A renewal of a non-running subscription changes nothing ------------------------
    {
        Row R;
        // Never answered: the row is set but not running.
        R.Lease.OnSend(500);
        CHECK(!R.Lease.Served());
        // The same reply shape means INSTALL for this row - the one case that may baseline.
        CHECK_EQ(SubEdgeAnswer(R.Lease.Served()) == SubEdgeAnswerKind::Install, 1);
        // Unserved retries fast: an install has to reach its provider now, not in a minute.
        CHECK_EQ(R.Lease.Interval(), SUB_RETRY_MS);
        // A renewal cannot arrive for a row that was never running (there is nothing to extend), so
        // the "changes nothing" half is the provider's: an existing entry is only given the new
        // timeout, and a NEW entry is the only thing a 0401 may create. Both are asserted on the
        // provider's call sites by run.sh.
        CHECK_EQ(R.Edge.Pending, 0);
        CHECK(!R.Edge.HaveBaseline);
    }

    // ---- 2b. the provider: an identical re-install extends, a changed table resets --------
    {
        // The rig's row: TRID 0x1008, source Dynamic.0 field 4 (0xFC000400), EdgeRising.
        const uint32_t Src = 0xFC000400u;
        const uint32_t OtherSrc = 0xFC000500u;
        const TriggerType Rising = TriggerType::EdgeRising;
        const TriggerType Falling = TriggerType::EdgeFalling;

        // A renewal: 0401 with the SAME entry, no explicit Set. Nothing may be reset, so the
        // provider keeps Hash (the edge counter), LastBool, LastSentMs and EdgeDue across it -
        // which is the whole point of the ruling: the counter stays continuous for the requester.
        CHECK(!SubProviderReinstallResets(false, false, Src, Rising, Src, Rising));

        // A new entry has no state worth keeping (requesterAddr 0 is the "free slot" marker).
        CHECK(SubProviderReinstallResets(false, true, Src, Rising, Src, Rising));

        // A retargeted source: the hash/counter/last value describe the field that is gone.
        CHECK(SubProviderReinstallResets(false, false, Src, Rising, OtherSrc, Rising));

        // A changed trigger: an edge counter and a value hash are not the same accounting, and
        // LastBool/EdgeDue only mean something for the edge triggers.
        CHECK(SubProviderReinstallResets(false, false, Src, Rising, Src, Falling));
        CHECK(SubProviderReinstallResets(false, false, Src, Rising, Src, TriggerType::OnChangeConfirm));

        // The explicit 0421 Set resets whatever the table says - it is a deliberate re-install,
        // never a renewal.
        CHECK(SubProviderReinstallResets(true, false, Src, Rising, Src, Rising));
        CHECK(SubProviderReinstallResets(true, true, Src, Rising, OtherSrc, Falling));
    }

    // ---- 3. A failed renewal leaves the row intact; a later one succeeds -------------------
    {
        Row R;
        R.Lease.OnSend(1000);
        Answer(R, 1100); // served
        R.Edge.Rebaseline(3);

        uint32_t Now = 1100 + SUB_KEEPALIVE_MS;
        SendRenewal(R, Now); // the first unanswered renewal
        for (int i = 0; i < SUB_FAST_ATTEMPTS; i++) {
            Now += R.Lease.Interval();
            SendRenewal(R, Now);
        }
        // The burst is spent: from here the row retries on the keepalive and is still there.
        CHECK(!R.Lease.FastWindowOpen());
        CHECK_EQ(R.Lease.Interval(), SUB_KEEPALIVE_MS);
        CHECK_EQ(R.Lease.Unanswered, 1 + SUB_FAST_ATTEMPTS);
        CHECK(R.Lease.Served());          // never un-served by a failed renewal
        CHECK_EQ(R.Edge.Last, 3);         // the counter accounting is untouched
        CHECK(R.Edge.HaveBaseline);

        // Six more keepalive-spaced renewals change nothing about the row's life.
        for (int i = 0; i < 6; i++) {
            Now += SUB_KEEPALIVE_MS;
            SendRenewal(R, Now);
            CHECK_EQ(R.Lease.Interval(), SUB_KEEPALIVE_MS);
            CHECK(R.Lease.Served());
        }

        // A later renewal that IS answered recovers the steady state by itself.
        Now += SUB_KEEPALIVE_MS;
        SendRenewal(R, Now);
        Answer(R, Now + 25);
        CHECK_EQ(R.Lease.Unanswered, 0);
        CHECK(R.Lease.FastWindowOpen());
        CHECK_EQ(R.Lease.Interval(), SUB_KEEPALIVE_MS);
        CHECK_EQ(R.Lease.LastValueMs, Now + 25);
        // The press that was counted while the renewals went unanswered is still compared.
        CHECK_EQ((int)R.Edge.Arrive(4, SubEdgeAnswer(R.Lease.Served()) == SubEdgeAnswerKind::Install),
                 (int)SubEdgeKind::Next);
    }

    // ---- 4. press -> renewal (button held) -> release+press still delivers both presses --
    {
        Row R;
        R.Lease.OnSend(1000);
        Answer(R, 1050);                  // install answered: the row is running
        R.Edge.Rebaseline(14);            // install reply: counter 14 = lifted, the known state
        R.Timeout = SubTimeoutFrom(1050);
        CHECK(R.Lease.Served());

        // The press is delivered live: delta 1, parity 1, nothing owed. (A live Next writes the
        // arriving counter's level directly; ReplayNext only moves in a walk.)
        CHECK_EQ((int)R.Edge.Arrive(15, SubEdgeAnswer(R.Lease.Served()) == SubEdgeAnswerKind::Install),
                 (int)SubEdgeKind::Next);
        CHECK_EQ(R.Edge.Last, 15);
        CHECK_EQ(SubEdgeLevelByte(15), 1); // counter 15: pressed

        // The renewal goes out and comes back while the button is held. It may extend only.
        const uint32_t RenewAt = 1000 + SUB_KEEPALIVE_MS;
        SendRenewal(R, RenewAt);
        CHECK_EQ(SubEdgeAnswer(R.Lease.Served()) == SubEdgeAnswerKind::Renew, 1);
        Answer(R, RenewAt + 40);
        R.Timeout = SubTimeoutFrom(RenewAt + 40);
        CHECK_EQ(R.Edge.Last, 15);        // the press above is still the counter accounting
        CHECK(R.Edge.HaveBaseline);
        CHECK_EQ(R.Edge.Pending, 0);      // the renewal's answer owed nothing
        CHECK_EQ(R.Lease.Unanswered, 0);
        CHECK_EQ(R.Timeout, RenewAt + 40 + SUB_TIMEOUT_MS);

        // Release + the next press arrive together as counter 17: one edge was never seen, and the
        // two levels it implies are delivered in order (16 lifted -> 0, then 17 pressed -> 1),
        // ending on the newest counter. Neither the renewal nor the gap loses the first press.
        CHECK_EQ((int)R.Edge.Arrive(17, SubEdgeAnswer(R.Lease.Served()) == SubEdgeAnswerKind::Install),
                 (int)SubEdgeKind::Gap);
        CHECK_EQ(R.Edge.Missing, 1);
        CHECK_EQ(R.Edge.Pending, 2);
        CHECK_EQ(R.Edge.ReplayLevel(), 0);
        R.Edge.OnReplayed();
        CHECK_EQ(R.Edge.ReplayLevel(), 1);
        R.Edge.OnReplayed();
        CHECK_EQ(R.Edge.Pending, 0);
        CHECK_EQ(R.Edge.Last, 17);
        CHECK(!R.Edge.Episode);
    }

    // ---- the cadence beats the provider's 120 s lease with margin --------------------------
    {
        // Two keepalives fit inside the provider's timeout, so one lost renewal cannot let the
        // provider's entry lapse before the next one lands.
        CHECK(SUB_KEEPALIVE_MS * 2 <= SUB_TIMEOUT_MS);
        CHECK(SUB_RETRY_MS < SUB_KEEPALIVE_MS);
    }

    // ---- a provider that really restarted is still a resync, not a huge gap ----------------
    {
        // The renewal rule must not swallow the node-reboot case: a RAM-only provider table means
        // the counter starts again at 0, which is a backward jump and is read as a restart.
        SubEdgeState Edge;
        Edge.Rebaseline(14);
        CHECK_EQ((int)Edge.Arrive(1, false), (int)SubEdgeKind::Resync);
        CHECK_EQ(Edge.Last, 1);
        CHECK_EQ(Edge.Pending, 0); // nothing is counted as lost: the count restarted
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
