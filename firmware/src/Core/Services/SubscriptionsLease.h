#pragma once

// Re-registration timing for a requester subscription entry.
//
// Split out of SubscriptionsDefs.h (which pulls the packet/register/storage layer and cannot be
// compiled on the host) so a native test can drive the real state machine: RequesterEntry embeds
// SubRequesterLease and the device code only calls its methods, so test/native/lease_test.cpp
// exercises the same decision that runs on the core, not a copy of it.

#include <cstdint>

// Docs/Services/Subscriptions.md: "Timeout 120s, renewed with new request." Both sides expire
// entries that stop being renewed (a value update / re-registration refreshes the deadline).
static constexpr uint32_t SUB_TIMEOUT_MS = 120000u;

// Re-registration cadence.
//   SUB_RETRY_MS     - while the provider has not answered (see SubRequesterLease::Interval)
//   SUB_KEEPALIVE_MS - renew the provider's 120 s lease once it does answer. Must stay well under
//                      SUB_TIMEOUT_MS: one lost keepalive has to leave room for the next one.
static constexpr uint32_t SUB_RETRY_MS = 100u;
static constexpr uint32_t SUB_KEEPALIVE_MS = 60000u;

// Fast re-sends allowed after a keepalive goes unanswered. Bounded on purpose: a provider that is
// simply gone (node powered off) must not get a 100 ms install loop on the shared bus - three
// retries repair a frame lost in the silence-gated half-duplex transmit, anything more is an
// outage, and an outage is the keepalive's business.
static constexpr uint8_t SUB_FAST_ATTEMPTS = 3u;

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
// Nothing here extends a lease optimistically: the entry's own 120 s deadline (RequesterEntry::
// Timeout) is renewed by the answer (ApplyRequesterValue), never by the send, so an install the
// provider never confirms is not a renewal and the entry expires on it.
struct SubRequesterLease {
    uint32_t LastRegisteredMs = 0; // when the last install/keepalive was sent
    uint32_t LastValueMs = 0;      // when the provider last answered (0 = never served)
    uint8_t Unanswered = 0;        // installs sent since that answer (saturating)

    bool Served() const { return LastValueMs != 0; }

    // An install/keepalive went out; until the provider answers, it may have been lost.
    void OnSend(uint32_t NowMs) {
        LastRegisteredMs = NowMs;
        if (Unanswered < 0xFF) Unanswered++;
    }

    // The provider answered (its reply carries the current value, which is applied through the
    // normal value path): the entry is served and its next renewal is the slow keepalive again.
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
    //  - never served: keep retrying fast. The provider's table is RAM-only and may still be
    //    empty, so there is nothing to wait for.
    //  - served, last sends unanswered: retry fast. A lost frame is only repaired by re-sending,
    //    and waiting the whole keepalive lets the provider's 120 s lease run out in the meantime -
    //    the node then drops the entry and stops pushing until the next keepalive re-installs it.
    //    That is the "switch dies at random, comes back after a while" failure.
    //  - served, answered: the slow keepalive (unchanged; not shortened globally).
    uint32_t Interval() const {
        if (!Served()) return SUB_RETRY_MS;
        if (Unanswered == 0) return SUB_KEEPALIVE_MS;
        return (Unanswered <= SUB_FAST_ATTEMPTS) ? SUB_RETRY_MS : SUB_KEEPALIVE_MS;
    }

    bool Due(uint32_t NowMs) const { return (uint32_t)(NowMs - LastRegisteredMs) >= Interval(); }
};
