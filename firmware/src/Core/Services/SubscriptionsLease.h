#pragma once

// Re-registration timing for a requester subscription entry.
//
// Split out of SubscriptionsDefs.h (which pulls the packet/register/storage layer and cannot be
// compiled on the host) so a native test can drive the real state machine: RequesterEntry embeds
// SubRequesterLease and the device code only calls its methods, so test/native/lease_test.cpp
// exercises the same decision that runs on the core, not a copy of it.

#include <cstdint>

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
    // reply.
    void OnAnswer(uint32_t NowMs) {
        LastValueMs = NowMs;
        Unanswered = 0;
    }

    // The provider's table is gone (the node re-appeared and its table is RAM-only): treat the
    // entry as unserved again so it re-enters the fast window instead of sitting on the slow
    // keepalive while the node's start-up install is retried.
    void OnProviderGone() {
        LastValueMs = 0;
        Unanswered = 0;
    }

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
        return (Unanswered <= SUB_FAST_ATTEMPTS) ? SUB_RETRY_MS : SUB_KEEPALIVE_MS;
    }

    bool Due(uint32_t NowMs) const { return (uint32_t)(NowMs - LastRegisteredMs) >= Interval(); }
};
