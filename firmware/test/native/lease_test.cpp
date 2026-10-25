// Host-side test for the requester subscription row's lifetime and install/keepalive cadence
// (Core/Services/SubscriptionsLease.h).
//
// Card t_eb35360c (owner ruling, 2026-10-24) replaces the per-entry lease death with:
//   1. the requester NEVER drops a row on time - a row lives until an explicit cancel, an orphan
//      announcement from the node, or a new setup. The provider keeps its own 120 s timeout and
//      drops its entry: a missing provider is a provider problem, not a reason to destroy the
//      requester's configuration (that is what made a node off for 3 minutes need a core reboot);
//   2. an install the provider does not answer is retried fast (SUB_RETRY_MS), whether or not the
//      subscription was ever served - the "switch dies at random" case (card t_6dcaad54) is a
//      served row that stops being answered;
//   3. the fast retries are capped at SUB_FAST_ATTEMPTS (5): past that, the SUB_KEEPALIVE_MS
//      cadence takes over until an install IS answered, and a confirmation (or a value update)
//      resets the counter to 0. The counter is SubRequesterLease::Unanswered: it lives in the live
//      RequesterEntry (after the 28 B wire prefix) and is NOT persisted - the `.SUBREQ` copy is
//      24 B per entry, so every boot/recall starts it at 0.
//
// The decision under test is the real one the device runs: RequesterEntry embeds SubRequesterLease
// and RequesterInitCheck only calls Lease.Due(now), so this file drives the same code, not a copy.
// The simulation below stands in for the two sides that matter - the requester row (kept until an
// explicit cancel) and the provider entry on the node (dropped by its own 120 s timeout) - with the
// real timeout/interval constants.
//
// The one acceptance criterion a host test cannot reach is (d) "an explicit cancel still removes
// the row": that is RequesterSet's cancel branch, which pulls the packet/register/storage layer and
// does not compile on the host. test/native/run.sh asserts it structurally instead (RequesterRemove
// is called from that branch and from nowhere else, in particular not from Tick).
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdint>
#include <cstdio>

#include "Core/Services/SubscriptionsLease.h"

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

// The install time used by every simulation: a real install happens after the boot's net settle,
// so uptime is well past 0 there (LastValueMs == 0 means "never served", so an answer at t=0 would
// read as unserved - an artefact of the model, not of the device).
static const uint32_t T0 = 5000;

// ---------------------------------------------------------------------------
// The two sides, simulated at 1 ms per tick (a superset of the main loop's rate).
// ---------------------------------------------------------------------------
struct Sim {
    SubRequesterLease Lease;     // core side: the requester row's bookkeeping
    uint32_t Now = 0;
    uint32_t Sends = 0;          // installs/keepalives sent (RequesterInitCheck)
    uint32_t Answers = 0;        // sends the provider answered with its current value
    uint32_t LastSendMs = 0;
    uint32_t MaxSendGapMs = 0;   // longest gap between two sends (the cadence actually seen)
    uint32_t FastSends = 0;      // sends issued with the fast interval in front of them
    uint32_t LastAnswerMs = 0;
    uint32_t PowerOnMs = 0;
    uint32_t DropSendNo = 0;     // 1-based send number that is lost in transit (0 = none)
    uint32_t DropGapMs = 0;      // gap from that lost send to the next one

    // Native side. Its provider table is RAM-only ("active until cancelled"), so powering the node
    // off loses its entry, and its own 120 s timeout drops an entry that stops being renewed.
    bool NodeOn = true;
    bool NodeHasEntry = false;
    uint32_t NodeTimeout = 0;
    uint32_t NodeExpiries = 0;

    void PowerOff() {
        NodeOn = false;
        NodeHasEntry = false;
    }
    void PowerOn() {
        NodeOn = true;
        NodeHasEntry = false; // empty table until an install lands
        PowerOnMs = Now;
    }

    // One install/keepalive, as RequesterInitCheck issues it. Only an ARRIVED frame changes the
    // node's state (its Change was applied, so its lease starts over, and its CID 1 reply carries
    // the current value, which is the answer that resets the requester's counter).
    void Send() {
        const uint32_t No = Sends + 1;
        if (Sends > 0) {
            uint32_t Gap = Now - LastSendMs;
            if (Gap > MaxSendGapMs) MaxSendGapMs = Gap;
            if (Gap < SUB_KEEPALIVE_MS) FastSends++;
            if (Sends == DropSendNo) DropGapMs = Gap; // how long after the lost frame it retried
        }
        Sends++;
        LastSendMs = Now;
        Lease.OnSend(Now);
        if (!NodeOn || No == DropSendNo) return;  // nothing hears it
        NodeHasEntry = true;
        NodeTimeout = SubTimeoutFrom(Now);
        Lease.OnAnswer(Now);
        Answers++;
        LastAnswerMs = Now;
    }

    // The install a Set / boot restore / node re-appearance sends (never the retry path).
    void Install() {
        Now = T0;
        Send();
    }
};

// Models SubscriptionsService::Tick + RequesterInitCheck, in that order. The PROVIDER's timeout
// still expires its own entry; the requester's row has no expiry any more, so there is nothing to
// model on that side - the row is only asked whether its next send is due.
template <typename Policy>
static void Run(Sim &S, uint32_t UntilMs, Policy OnTick) {
    for (; S.Now <= UntilMs; S.Now++) {
        OnTick(S); // power events, the node announcing itself after a reboot
        if (S.NodeHasEntry && (int32_t)(S.Now - S.NodeTimeout) >= 0) {
            S.NodeHasEntry = false;
            S.NodeExpiries++;
        }
        if (S.Lease.Due(S.Now)) S.Send();
    }
}

// The fast re-sends a lease issues from the state it is in, until it falls back to the keepalive.
static uint32_t CountFastResends(SubRequesterLease &L, uint32_t &Now) {
    uint32_t Resends = 0;
    while (L.Interval() == SUB_RETRY_MS && Resends < 0xFF) {
        Now += L.Interval();
        L.OnSend(Now);
        Resends++;
    }
    return Resends;
}

int main() {
    // ---- The cadence has to fit inside the lease it renews: one lost keepalive plus the fast
    // repairs must still land before the provider's 120 s lease runs out.
    CHECK(SUB_RETRY_MS < SUB_KEEPALIVE_MS);
    CHECK(SUB_KEEPALIVE_MS < SUB_TIMEOUT_MS);
    CHECK(SUB_KEEPALIVE_MS + SUB_RETRY_MS * (SUB_FAST_ATTEMPTS + 1) < SUB_TIMEOUT_MS);
    CHECK_EQ(SUB_FAST_ATTEMPTS, 5); // the ruling's cap

    // ---- An unanswered install must re-send after ~SUB_RETRY_MS, not after the keepalive.
    {
        SubRequesterLease L;
        CHECK(!L.Served());
        // A row that was just set or recalled has sent nothing yet, so its first install is due at
        // once - waiting the keepalive here would leave a Recall's subscriptions uninstalled for a
        // minute.
        CHECK_EQ(L.Interval(), SUB_RETRY_MS);
        CHECK(L.Due(10000));     // any uptime past SUB_RETRY_MS

        L.OnSend(1000);          // install goes out
        CHECK(!L.Served());
        CHECK_EQ(L.Unanswered, 1);
        CHECK_EQ(L.Interval(), SUB_RETRY_MS);
        CHECK(L.Due(1000 + SUB_RETRY_MS));
        CHECK(!L.Due(1000 + SUB_RETRY_MS - 1));

        L.OnAnswer(1010);        // the CID 1 reply arrives with the current value
        CHECK(L.Served());
        CHECK_EQ(L.Unanswered, 0);
        CHECK_EQ(L.Interval(), SUB_KEEPALIVE_MS);
        // The keepalive countdown runs from the SEND, not from the answer.
        CHECK(!L.Due(1000 + SUB_KEEPALIVE_MS - 1));
        CHECK(L.Due(1000 + SUB_KEEPALIVE_MS));
    }

    // ---- (b) A served row whose keepalive goes unanswered repairs fast, and the fast retries are
    // capped at five: after that the keepalive cadence takes over, so a provider that is simply
    // gone is not hammered at 100 ms on the shared bus.
    {
        SubRequesterLease L;
        L.OnSend(0);
        L.OnAnswer(5);
        uint32_t Now = 5;
        CHECK_EQ(L.Interval(), SUB_KEEPALIVE_MS);

        Now += SUB_KEEPALIVE_MS; // the keepalive is lost
        L.OnSend(Now);
        CHECK_EQ(L.Unanswered, 1);
        CHECK_EQ(L.Interval(), SUB_RETRY_MS); // the point of the fix (t_6dcaad54)
        CHECK(L.Due(Now + SUB_RETRY_MS));
        CHECK(!L.Due(Now + SUB_RETRY_MS - 1));

        // Exactly five fast re-sends, then the keepalive.
        CHECK_EQ(CountFastResends(L, Now), SUB_FAST_ATTEMPTS);
        CHECK_EQ(L.Unanswered, 1 + SUB_FAST_ATTEMPTS);
        CHECK_EQ(L.Interval(), SUB_KEEPALIVE_MS);
        CHECK(!L.Due(Now + SUB_RETRY_MS));            // no 100 ms loop past the cap
        CHECK(L.Due(Now + SUB_KEEPALIVE_MS));
    }

    // ---- (c) A confirmation resets the counter: the next lost keepalive gets a whole fresh fast
    // window, not the one-or-two retries left over from the previous outage. (OnAnswer is what
    // ApplyRequesterValue calls - a value update and the CID 1 install reply both end up there.)
    {
        SubRequesterLease L;
        L.OnSend(0);
        L.OnAnswer(5);
        uint32_t Now = 5 + SUB_KEEPALIVE_MS;
        L.OnSend(Now);                          // keepalive lost
        Now += SUB_RETRY_MS;
        L.OnSend(Now);                          // re-send 1
        Now += SUB_RETRY_MS;
        L.OnSend(Now);                          // re-send 2
        CHECK_EQ(L.Unanswered, 3);

        L.OnAnswer(Now + 1);                    // the provider answers after all
        CHECK_EQ(L.Unanswered, 0);
        CHECK(L.Served());
        CHECK_EQ(L.Interval(), SUB_KEEPALIVE_MS);

        Now = Now + 1 + SUB_KEEPALIVE_MS;
        L.OnSend(Now);                          // the NEXT keepalive is lost too
        CHECK_EQ(L.Unanswered, 1);
        CHECK_EQ(CountFastResends(L, Now), SUB_FAST_ATTEMPTS); // the whole window again
    }

    // ---- The node re-appeared (its RAM-only table is gone): the row is treated as unserved again
    // and re-enters the fast window instead of sitting on the slow keepalive.
    {
        SubRequesterLease L;
        L.OnSend(0);
        L.OnAnswer(5);
        CHECK_EQ(L.Interval(), SUB_KEEPALIVE_MS);
        L.OnProviderGone();
        CHECK(!L.Served());
        L.OnSend(6);                 // ReRegisterForNode sends at once
        CHECK_EQ(L.Interval(), SUB_RETRY_MS);
    }

    // ---- Idle edge subscription: one install, then the source never changes again, so no value
    // ever flows. Both sides must still survive on the keepalives alone (the shape that must NOT
    // regress: shortening the keepalive, or keying it to value updates, kills it). Each keepalive is
    // ANSWERED - the CID 1 reply carries the current value even when the trigger, an edge, has
    // nothing to report.
    {
        Sim S;
        S.Install();
        Run(S, T0 + 190000, [](Sim &) {});
        CHECK(S.NodeHasEntry);
        CHECK(S.Lease.Served());
        CHECK_EQ(S.Lease.Interval(), SUB_KEEPALIVE_MS);
        CHECK_EQ(S.Sends, 4);                      // install + keepalives at 60/120/180 s
        CHECK_EQ(S.MaxSendGapMs, SUB_KEEPALIVE_MS);
        CHECK_EQ(S.NodeExpiries, 0);
    }

    // ---- The bug (t_6dcaad54): a keepalive is lost (send #2, at 60 s). The next attempt must be
    // SUB_RETRY_MS, not the keepalive - the node's lease runs out in exactly that window.
    {
        Sim S;
        S.Install();
        S.DropSendNo = 2;                    // the keepalive never arrives
        Run(S, T0 + 130000, [](Sim &) {});
        CHECK_EQ(S.DropGapMs, SUB_RETRY_MS);  // repaired at once, not at the next keepalive
        CHECK_EQ(S.Sends, 4);                 // install, lost keepalive, repair, next keepalive
        CHECK_EQ(S.NodeExpiries, 0);          // the node kept its entry throughout
        CHECK(S.NodeHasEntry);
    }

    // ---- (a) A three-minute provider outage. The row is NOT dropped (no requester-side expiry, no
    // reboot, no setup re-apply): the retries continue on the capped cadence for the whole outage,
    // and the row re-installs once the node is back. Worst case here is a node that comes back
    // silently - the re-install then waits for the next keepalive, which is the bound asserted.
    {
        Sim S;
        S.Install();                                  // install lands
        CHECK_EQ(S.Answers, 1);

        Run(S, T0 + 181000, [](Sim &Self) {           // node off at 1 s, back at 181 s
            if (Self.Now == T0 + 1000) Self.PowerOff();
            if (Self.Now == T0 + 181000) Self.PowerOn();
        });
        CHECK(!S.NodeHasEntry);                       // its RAM-only table is gone
        // The row survived the whole outage and kept trying, on the bounded cadence.
        CHECK(S.Sends >= 1 + 1 + SUB_FAST_ATTEMPTS);
        CHECK(S.MaxSendGapMs <= SUB_KEEPALIVE_MS);    // never a 100 ms loop across it
        CHECK(S.FastSends <= SUB_FAST_ATTEMPTS);      // the fast window was spent once, not per minute

        // The node is back with an empty table; the row is still there and re-installs it.
        Run(S, T0 + 300000, [](Sim &) {});
        CHECK(S.NodeHasEntry);                        // re-installed: no reboot, no re-Set
        CHECK(S.Lease.Served());
        CHECK(S.LastAnswerMs >= S.PowerOnMs);
        CHECK(S.LastAnswerMs - S.PowerOnMs <= SUB_KEEPALIVE_MS); // within one keepalive of the return
        CHECK_EQ(S.NodeExpiries, 0);
    }

    // ---- (a) with the node announcing itself after its reboot (the normal case: Device.h's
    // discover handler queues a re-push, ReRegisterForNode marks the row unserved and sends at
    // once), the dead window is one send rather than one keepalive.
    {
        Sim S;
        S.Install();
        Run(S, T0 + 181000, [](Sim &Self) {
            if (Self.Now == T0 + 1000) Self.PowerOff();
            if (Self.Now == T0 + 181000) {
                Self.PowerOn();
                Self.Lease.OnProviderGone(); // ReRegisterForNode: the node's table is RAM-only
                Self.Send();                 // ... and the re-push goes out in the same tick
            }
        });
        CHECK(S.NodeHasEntry);
        CHECK(S.Lease.Served());
        CHECK_EQ(S.LastAnswerMs, T0 + 181000);
        CHECK_EQ(S.NodeExpiries, 0);
    }

    // ---- A provider that never answers at all: the row is still not dropped, but it is not
    // retried at 100 ms for ever either. Five fast retries, then the keepalive, for as long as it
    // takes - ten minutes here.
    {
        Sim S;
        S.PowerOff();               // nothing ever hears the install
        S.Install();
        Run(S, T0 + 600000, [](Sim &) {});
        CHECK(!S.Lease.Served());
        CHECK(!S.NodeHasEntry);
        CHECK_EQ(S.Answers, 0);
        CHECK(S.Lease.Unanswered >= 10);        // ten minutes of keepalives, saturating counter
        CHECK(S.MaxSendGapMs == SUB_KEEPALIVE_MS); // steady state: the keepalive, not 100 ms
        CHECK_EQ(S.FastSends, SUB_FAST_ATTEMPTS);  // the fast window was spent once
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
