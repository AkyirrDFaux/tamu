#pragma once

// Re-registration timing for a requester subscription entry.
//
// Split out of SubscriptionsDefs.h (which pulls the packet/register/storage layer and cannot be
// compiled on the host) so a native test can drive the real state machine: RequesterEntry embeds
// SubRequesterLease and the device code only calls its methods, so test/native/lease_test.cpp
// exercises the same decision that runs on the core, not a copy of it.

#include <cstdint>
#include <cstdio>

// Docs/Services/Subscriptions.md: "Timeout 120s, renewed with new request." The PROVIDER expires an
// entry that stops being renewed (SubscriptionsControl.h Tick, SubscriptionsProvider.h). The
// requester does NOT: per the owner's ruling (t_eb35360c, 2026-10-24) a row lives until an explicit
// cancel, an orphan announcement from the node, or a new setup. The asymmetry is deliberate - a
// missing provider is a provider problem, and destroying the requester's configuration over it is
// what made a node that was off for two minutes need a core reboot. The requester entry still
// CARRIES the 120 s deadline (it is part of the 28 B wire entry and is refreshed on an answer), but
// nothing expires a row on it; see SubRequesterLease::Interval for what does bound the retries.
static constexpr uint32_t SUB_TIMEOUT_MS = 120000u;

// Re-registration cadence.
//   SUB_RETRY_MS     - while the provider has not answered (see SubRequesterLease::Interval)
//   SUB_KEEPALIVE_MS - renew the provider's 120 s lease once it does answer. Must stay well under
//                      SUB_TIMEOUT_MS: one lost keepalive has to leave room for the next one.
static constexpr uint32_t SUB_RETRY_MS = 100u;
static constexpr uint32_t SUB_KEEPALIVE_MS = 60000u;

// Fast re-sends allowed after an install goes unanswered. Bounded on purpose, and now the only
// thing that bounds it: the requester no longer expires the row that does the retrying, so without
// this cap a provider that is simply gone (node powered off) would get a 100 ms install loop on the
// shared bus for ever. Five retries repair a frame lost in the silence-gated half-duplex transmit;
// past that it is an outage, and an outage is the keepalive's business.
static constexpr uint8_t SUB_FAST_ATTEMPTS = 5u;

// Docs: the local deadline an installed entry is given (not persisted).
static inline uint32_t SubTimeoutFrom(uint32_t NowMs) { return NowMs + SUB_TIMEOUT_MS; }

// The give-up report: one line per episode, when a row's fast retry window is spent (card
// t_62e8f3d3). Five short renewals went unanswered, so from here the row is retried at the
// keepalive cadence - and until this line existed nothing in the log said why a row went quiet.
//
// The text lives here, next to the state that decides it and host-clean, for the same reason the
// lease itself does: test/native/lease_test.cpp formats the very string the device prints instead
// of a copy of it, and the argument order has one owner (SubFormatGaveUp), so the emit site cannot
// pass the fields in the wrong order.
static constexpr char SUB_GAVE_UP_TAG[] = "SUB";
static constexpr char SUB_GAVE_UP_FMT[] =
    "TRID %04X prov %04X reg %08X: %u short renewals unanswered, uptime %u ms, row kept, "
    "retrying at the keepalive";
// DeviceLog's own buffer (Devices/*/Log.h): the longest form of the line above has to fit inside
// it, and the host test pins that it does.
static constexpr unsigned SUB_GAVE_UP_TEXT_MAX = 128;

// Formats the line (returns the length snprintf produced, like snprintf itself, so a truncated
// line is visible as Len >= Size).
static inline unsigned SubFormatGaveUp(char *Dst, unsigned Size, uint16_t Trid, uint16_t ProviderAddr,
                                       uint32_t TargetReg, unsigned Renewals, uint32_t UptimeMs) {
    int Len = snprintf(Dst, Size, SUB_GAVE_UP_FMT, (unsigned)Trid, (unsigned)ProviderAddr,
                       (unsigned)TargetReg, Renewals, (unsigned)UptimeMs);
    return (Len < 0) ? Size : (unsigned)Len;
}

// The requester entry's install/keepalive bookkeeping (transient: not part of the wire entry).
//
// An install is only known to have landed when the provider answers it. The answer is the CID 1
// reply itself - the provider replies with the CURRENT VALUE (SubscriptionsControl.h, SubCid::
// Change), which carries no FLAG_SUCCESS: a FLAG_SUCCESS/FLAG_FAIL status ack is routed to the app
// link by Dispatcher.h and never reaches the requester. So any typed packet for this TRID from
// this provider that arrives through the value path is the answer.
//
// Nothing here extends a lease optimistically either: the entry's 120 s deadline
// (RequesterEntry::Timeout) is refreshed only by the answer (ApplyRequesterValue), never by the send.
// That deadline does not destroy the row any more - it is carried for the wire and the app - so an
// unanswered install now costs a bounded retry window instead of the subscription.
//
// Where the state lives: SubRequesterLease is embedded in RequesterEntry *after* the 28 B wire
// prefix (SubscriptionsDefs.h), so it is neither streamed by Get subscriptions nor written to
// `.SUBREQ` (24 B per entry). It is RAM-only and therefore resets on every boot, recall or Set -
// a restored row starts unserved with a full fast window, which is exactly what its re-registration
// wants.
struct SubRequesterLease {
    uint32_t LastRegisteredMs = 0; // when the last install/keepalive was sent
    // When the provider last answered (0 = never served). The ruling's "activated at least once"
    // state: an answered row renews on the keepalive, while a never-served one has to reach its
    // provider first (Interval below).
    uint32_t LastValueMs = 0;
    uint8_t Unanswered = 0;        // installs sent since that answer (saturating, not persisted)
    // The give-up line has been emitted for the current unanswered episode. Once per episode, not
    // per retry: an answer (OnAnswer) ends the episode, so the next outage reports again. RAM-only,
    // in the same transient tail as Unanswered - it is neither streamed nor persisted (`.SUBREQ`
    // holds 24 B per entry), so a boot, recall or Set starts a fresh episode.
    bool GaveUpReported = false;

    bool Served() const { return LastValueMs != 0; }

    // An install/keepalive went out; until the provider answers, it may have been lost.
    void OnSend(uint32_t NowMs) {
        LastRegisteredMs = NowMs;
        if (Unanswered < 0xFF) Unanswered++;
    }

    // The provider answered (its reply carries the current value, which is applied through the
    // normal value path): the installs are answered, so the counter restarts and the next renewal
    // is the slow keepalive again. This is also the "a value update resets the counter" half of the
    // ruling - ApplyRequesterValue calls it for every value that arrives, not just for the install
    // reply - and it ends the give-up episode, so a later outage reports again.
    void OnAnswer(uint32_t NowMs) {
        LastValueMs = NowMs;
        Unanswered = 0;
        GaveUpReported = false;
    }

    // The provider's table is gone (the node re-appeared and its table is RAM-only): treat the
    // entry as unserved again so it re-enters the fast window instead of sitting on the slow
    // keepalive while the node's start-up install is retried. That is a new episode as well.
    void OnProviderGone() {
        LastValueMs = 0;
        Unanswered = 0;
        GaveUpReported = false;
    }

    // Is the short-retry window still open? The single place that answers it, because both the
    // cadence (Interval) and the give-up report key off the same boundary - computing it twice is
    // how a row ends up retrying on the keepalive without the line that explains it.
    //   Unanswered counts the installs sent since the last answer, and SUB_FAST_ATTEMPTS of them are
    //   renewed at the short cadence past the one that opened the episode (the keepalive of a served
    //   row, or the initial install of a new one). So the window closes on the send that spends the
    //   fifth short renewal, and the NEXT send is the keepalive.
    bool FastWindowOpen() const { return Unanswered <= SUB_FAST_ATTEMPTS; }

    // The give-up edge: the fast window has just closed - the fifth short renewal went unanswered -
    // and this episode has not been reported yet. The row is NOT dropped (nothing on the requester
    // side drops it); from here it retries at the keepalive cadence until the provider answers.
    bool ShouldReportGaveUp() const { return !FastWindowOpen() && !GaveUpReported; }
    void OnGaveUpReported() { GaveUpReported = true; }

    // How long after the last send the next one is due.
    //
    //  - nothing in flight and the provider has answered (Unanswered == 0, Served()): the slow
    //    keepalive. This is the steady state of a working subscription.
    //  - nothing in flight and never served: retry fast. There is no send to wait for yet - a row
    //    that was just set or just recalled has to reach its provider now, not in 60 s - and the
    //    provider's table is RAM-only, so it may still be empty.
    //  - installs unanswered (1..SUB_FAST_ATTEMPTS): retry fast. A lost frame is only repaired by
    //    re-sending, and waiting the whole keepalive lets the provider's 120 s lease run out in the
    //    meantime - the node then drops the entry and stops pushing until the next keepalive
    //    re-installs it. That is the "switch dies at random, comes back after a while" failure.
    //    This covers a row that was served before and then stops being answered as well as one that
    //    has never been answered yet; the ruling makes no distinction between them.
    //  - past the cap: the slow keepalive until an install is answered. The requester keeps the row
    //    (nothing expires it), so the cap is what keeps a powered-off node from being retried at
    //    100 ms for ever on the shared bus.
    uint32_t Interval() const {
        if (Unanswered == 0 && Served()) return SUB_KEEPALIVE_MS;
        return FastWindowOpen() ? SUB_RETRY_MS : SUB_KEEPALIVE_MS;
    }

    bool Due(uint32_t NowMs) const { return (uint32_t)(NowMs - LastRegisteredMs) >= Interval(); }
};
