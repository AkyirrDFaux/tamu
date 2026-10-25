// Host-side test for the requester subscription's keepalive cadence
// (Core/Services/SubscriptionsLease.h).
//
// The failure this pins (card t_6dcaad54, "the bambi emote switch dies at random"): the core
// renewed the provider's 120 s lease on a flat 60 s keepalive once any value had flowed, and never
// asked whether the keepalive was answered. One keepalive lost in the half-duplex, silence-gated
// transmit, and the node's provider entry expired 120 s after its last renewal: its trigger
// (evaluated on the node, edge-only) then pushed nothing at all, so every switch press was
// invisible until the next keepalive re-installed the subscription - up to 60 s later. Random
// death, everything else healthy, recovery "after a while".
//
// The decision under test is the real one the device runs: RequesterEntry embeds SubRequesterLease
// and RequesterInitCheck only calls Lease.Due(now), so this file drives the same code, not a copy.
// The simulation below stands in for the two leases that matter - the core's requester entry
// (renewed by its own send) and the provider entry on the node (renewed only when the frame
// actually ARRIVES) - with the real timeout/interval constants.
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

// ---------------------------------------------------------------------------
// The two leases, simulated at 1 ms per tick (a superset of the main loop's rate).
// ---------------------------------------------------------------------------
struct Sim {
    SubRequesterLease Lease;      // core side: the requester entry
    bool CoreHasEntry = true;
    // core side: the entry's own 120 s deadline. Granted when the subscription is set/restored and
    // renewed ONLY by the provider's answer - never by a send (owner ruling: nothing is renewed
    // optimistically), so an entry nothing answers expires 120 s after its last answer.
    uint32_t CoreTimeout = SubTimeoutFrom(0);
    bool NodeHasEntry = false;
    uint32_t NodeTimeout = 0;     // provider side: the node's 120 s lease (renewed on ARRIVAL only)
    uint32_t Now = 0;
    uint32_t Sends = 0;
    uint32_t LastSendMs = 0;
    uint32_t MaxSendGapMs = 0;      // longest gap between two sends (the cadence actually seen)
    uint32_t SecondGapMs = 0;       // gap into send #2: the repair after the keepalive (if any)
    bool CoreExpired = false;
    bool NodeExpired = false;

    // One install/keepalive. Only an ARRIVED frame renews anything: the node's lease (its Change
    // was applied) and the core's deadline (its reply carries the value). An unanswered send moves
    // the retry clock and nothing else.
    void Send(bool Delivered) {
        if (Sends > 0) {
            uint32_t Gap = Now - LastSendMs;
            if (Gap > MaxSendGapMs) MaxSendGapMs = Gap;
        }
        if (Sends == 2) SecondGapMs = Now - LastSendMs; // gap into send #3: the repair
        Sends++;
        LastSendMs = Now;
        Lease.OnSend(Now);
        if (!Delivered) return;
        NodeHasEntry = true;
        NodeTimeout = SubTimeoutFrom(Now); // the node applied the Change: its lease starts over
        Lease.OnAnswer(Now);               // ... and its CID 1 reply carries the current value
        CoreTimeout = SubTimeoutFrom(Now); // the answer is what renews the requester's deadline
    }
};

// Runs the loop from Now to `UntilMs`. `Deliver(sim)` decides whether the send being made arrives;
// the reply is modelled with no latency, so "arrived" == "answered" (the async window is the
// SUB_RETRY_MS retry itself, which the assertions below cover).
template <typename Policy>
static void Run(Sim &S, uint32_t UntilMs, Policy Deliver) {
    for (; S.Now <= UntilMs; S.Now++) {
        // Same order as SubscriptionsService::Tick: provider expiry, requester expiry, then the
        // re-registration pass.
        if (S.NodeHasEntry && (int32_t)(S.Now - S.NodeTimeout) >= 0) {
            S.NodeHasEntry = false;
            S.NodeExpired = true;
        }
        if (S.CoreHasEntry && (int32_t)(S.Now - S.CoreTimeout) >= 0) {
            // Tick drops an expired entry from the table (the file keeps it), so it stops sending.
            S.CoreExpired = true;
            S.CoreHasEntry = false;
        }
        if (!S.CoreHasEntry || !S.Lease.Due(S.Now)) continue;
        S.Send(Deliver(S));
    }
}

int main() {
    // ---- The cadence has to fit inside the lease it renews: one lost keepalive plus the fast
    // repair must still land before the provider's 120 s lease runs out.
    CHECK(SUB_RETRY_MS < SUB_KEEPALIVE_MS);
    CHECK(SUB_KEEPALIVE_MS < SUB_TIMEOUT_MS);
    CHECK(SUB_KEEPALIVE_MS + SUB_RETRY_MS * (SUB_FAST_ATTEMPTS + 1) < SUB_TIMEOUT_MS);

    // ---- An unanswered install must re-send after ~SUB_RETRY_MS, not after the keepalive.
    {
        SubRequesterLease L;
        CHECK(!L.Served());
        CHECK_EQ(L.Interval(), SUB_RETRY_MS); // never served: unchanged fast retry

        L.OnSend(1000);          // install goes out
        CHECK(!L.Served());
        CHECK_EQ(L.Unanswered, 1);
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

    // ---- A served entry whose keepalive goes unanswered repairs fast, then stops hammering.
    // The expectations below are unrolled for SUB_FAST_ATTEMPTS == 3 (the fast window is the
    // keepalive itself plus three re-sends).
    {
        CHECK_EQ(SUB_FAST_ATTEMPTS, 3);
        SubRequesterLease L;
        L.OnSend(0);
        L.OnAnswer(5);
        const uint32_t Keepalive = 5 + SUB_KEEPALIVE_MS;
        CHECK_EQ(L.Interval(), SUB_KEEPALIVE_MS);

        L.OnSend(Keepalive); // the keepalive is lost
        CHECK_EQ(L.Unanswered, 1);
        CHECK_EQ(L.Interval(), SUB_RETRY_MS); // the point of the fix
        CHECK(L.Due(Keepalive + SUB_RETRY_MS));
        CHECK(!L.Due(Keepalive + SUB_RETRY_MS - 1));

        L.OnSend(Keepalive + SUB_RETRY_MS); // re-send 1
        CHECK_EQ(L.Interval(), SUB_RETRY_MS);
        L.OnSend(Keepalive + 2 * SUB_RETRY_MS); // re-send 2
        CHECK_EQ(L.Interval(), SUB_RETRY_MS);
        L.OnSend(Keepalive + 3 * SUB_RETRY_MS); // re-send 3
        CHECK_EQ(L.Unanswered, 4);
        // Bounded: a provider that is simply gone must not get a 100 ms install loop on the bus.
        CHECK_EQ(L.Interval(), SUB_KEEPALIVE_MS);
        CHECK(!L.Due(Keepalive + 4 * SUB_RETRY_MS));
        CHECK(L.Due(Keepalive + 3 * SUB_RETRY_MS + SUB_KEEPALIVE_MS));

        // ... and one answer puts it straight back on the slow keepalive.
        const uint32_t Answered = Keepalive + 3 * SUB_RETRY_MS + 1;
        L.OnAnswer(Answered);
        CHECK_EQ(L.Unanswered, 0);
        CHECK_EQ(L.Interval(), SUB_KEEPALIVE_MS);
        CHECK(L.Due(Answered + SUB_KEEPALIVE_MS));
    }

    // ---- The node re-appeared (its RAM-only table is gone): fast retry window again.
    {
        SubRequesterLease L;
        L.OnSend(0);
        L.OnAnswer(5);
        CHECK_EQ(L.Interval(), SUB_KEEPALIVE_MS);
        L.OnProviderGone();
        CHECK(!L.Served());
        CHECK_EQ(L.Interval(), SUB_RETRY_MS);
    }

    // ---- Idle edge subscription: one install, then the source never changes again, so no value
    // ever flows. Both 120 s leases must still survive on the keepalives alone (the shape that must
    // NOT regress: shortening the keepalive, or keying it to value updates, kills it). Each
    // keepalive is ANSWERED - the CID 1 reply carries the current value even when the trigger, an
    // edge, has nothing to report - and that answer is what renews the requester's deadline.
    {
        Sim S;
        Run(S, 190000, [](Sim &) { return true; });
        CHECK(!S.CoreExpired);
        CHECK(!S.NodeExpired);
        CHECK(S.NodeHasEntry);
        CHECK(S.Lease.Served());
        CHECK_EQ(S.Lease.Interval(), SUB_KEEPALIVE_MS);
        CHECK_EQ(S.Sends, 4);                    // install + keepalives at 60/120/180 s
        CHECK_EQ(S.MaxSendGapMs, SUB_KEEPALIVE_MS);
    }

    // ---- The bug: a keepalive is lost (send #2, at 60 s). Before the fix the next attempt WAS
    // the keepalive - 60 s later, exactly when the node's lease expires; now it is SUB_RETRY_MS.
    {
        Sim S;
        Run(S, 130000, [](Sim &Self) { return Self.Sends != 1; });
        CHECK_EQ(S.Sends, 4);                    // install, lost keepalive, repair, next keepalive
        CHECK_EQ(S.SecondGapMs, SUB_RETRY_MS);
        CHECK(!S.NodeExpired);                   // the node kept its entry the whole time
        CHECK(S.NodeHasEntry);
        CHECK(!S.CoreExpired);                   // and the repair's answer kept the core's entry
    }

    // ---- A provider that never answers. The entry is NOT renewed for having sent (nothing is
    // optimistic), so it expires one lease after its last answer and Tick drops it from the table -
    // the documented "expiry is a FAILURE" path, which leaves `.SUBREQ` alone. The retries stop
    // with it: no 100 ms install loop against a node that is simply gone.
    {
        Sim S;
        Run(S, 600000, [](Sim &Self) { return Self.Sends == 0; }); // one install, then silence
        CHECK(S.CoreExpired);
        CHECK(S.NodeExpired);                    // the node lost its entry too (it is gone)
        CHECK(!S.CoreHasEntry);                  // dropped from the live table, so it stopped
        CHECK(S.Sends <= 1 + (SUB_TIMEOUT_MS / SUB_KEEPALIVE_MS + 2) * (SUB_FAST_ATTEMPTS + 1));
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
