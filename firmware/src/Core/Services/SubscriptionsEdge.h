#pragma once

// The edge counter an edge-triggered value update carries, and what the requester does with it.
//
// An edge-triggered subscription is a *press*: the value is a Bool and the update exists to say
// "this happened". The wire used to carry the value alone, which made a press that arrived and a
// press that was lost look identical at the requester - the target already held the value, so a
// lost update changed nothing and said nothing (the "switch dies at random" family). The provider
// has always counted its edges (ProviderEntry::Hash, Docs "Counter: for Bool edges only"); the
// counter is now the update's whole payload, and the LEVEL travels as its PARITY:
//
//   value update payload, edge triggers only (Docs/Services/Subscriptions.md, "Value update"):
//
//     +--------+--------+
//     | ctr lo | ctr hi |
//     +--------+--------+
//       0        1        ...and nothing else: 2 bytes is the whole payload
//
//   - u16, little-endian: the low 16 bits of the provider's 32-bit edge counter, so the wire value
//     is `counter mod 2^16`. There is no separate value: for an edge trigger THE COUNTER IS THE
//     PAYLOAD.
//   - PARITY CARRIES THE LEVEL: odd = true (pressed / active), even = false (lifted). A counter that
//     advanced by n from the row's last one means n edges happened, and every one of them is a
//     level of its own (SubEdgeState's walk below delivers them in order, one per pass).
//   - the payload length is exactly SUB_EDGE_COUNTER_SIZE. Nothing is appended.
//   - every other trigger keeps its payload byte-for-byte: counter-free, value from byte 0.
//   - the ANSWER to "Change subscription" (0401) is unchanged and carries the current value alone
//     (Docs CID 1, "Current value"); for an edge row that is its Bool, one byte. A value is at
//     least one byte and a push is exactly two, so the shapes cannot collide. The answer is not a
//     press - it tells the row to forget its baseline, and the next push is the zero point. Leaving
//     it alone is also what keeps the feature inside the DAS's code space.
//
// Why the provider counts BOTH directions, whatever the trigger: parity can only mean the level if
// every level change moves the counter. A rising-only counter would stay odd after the release and
// the requester would read "still pressed" from a lifted switch. So the counter counts level
// changes and the trigger only decides which of them are SENT (SubEdgeClassify below).
//
// Why both ends must agree on the encoding: a mismatch (byte order, width, a value appended or not)
// does not fail loudly - every update just looks like a gap of a random size - so the encoding has
// exactly one owner (SubEdgeCounterStore/Load below) and both sides call it. The two images must be
// flashed together for the same reason: an old requester would write the counter's bytes into the
// target as if they were the value, and an old provider's counter-free push is a different layout.
//
// Host-clean (no device headers, no globals) so test/native/edge_counter_test.cpp drives the same
// state machine the core runs: RequesterEntry embeds SubEdgeState and the requester only calls its
// methods (Core/Services/SubscriptionsRequester.h). The requester half is behind USE_SUB_REQUEST,
// the flag that compiles the requester role at all - the DAS (provider only) pays nothing for it.

#include <cstdint>
#include <cstring>

#include "Core/Types/Enums.h"

// The whole edge payload: the counter, 16 bits. Nothing follows it.
static constexpr uint8_t SUB_EDGE_COUNTER_SIZE = 2;

// The provider's own counter is 32 bits (it increments ProviderEntry::Hash). Only the low 16 cross
// the wire, so this is the one place that decides what "the counter" means on the wire: a wrap at
// 2^16 looks exactly like a normal +1 to the requester (the classification below is modular), and
// nothing is lost as long as no more than SUB_EDGE_RESYNC_DELTA-1 edges go unseen across a gap.
static inline void SubEdgeCounterStore(uint8_t *Dst, uint32_t Counter) {
    Dst[0] = (uint8_t)(Counter & 0xFF);
    Dst[1] = (uint8_t)((Counter >> 8) & 0xFF);
}
static inline uint16_t SubEdgeCounterLoad(const uint8_t *Src) {
    return (uint16_t)((uint16_t)Src[0] | ((uint16_t)Src[1] << 8));
}
static_assert(SUB_EDGE_COUNTER_SIZE == 2, "the edge counter is a u16 on the wire");

// The level a counter carries: its parity. Odd = true/pressed, even = false/lifted.
static inline bool SubEdgeLevel(uint16_t Counter) { return (Counter & 1u) != 0; }

// The same level as the single byte a Bool target takes (the only thing that may be written to the
// target: the counter itself never is).
static inline uint8_t SubEdgeLevelByte(uint16_t Counter) { return SubEdgeLevel(Counter) ? 1 : 0; }

// Is this trigger an edge trigger (the only kind whose payload is the counter)?
// A range test, not three equalities: it is called at every provider send and on both ends, and the
// enum's edge values are contiguous (pinned below).
static inline bool SubTriggerIsEdge(TriggerType Trigger) {
    const uint8_t Value = (uint8_t)Trigger;
    return Value >= (uint8_t)TriggerType::EdgeRising && Value <= (uint8_t)TriggerType::EdgeAny;
}
static_assert((uint8_t)TriggerType::EdgeRising == 4 && (uint8_t)TriggerType::EdgeFalling == 5 &&
                  (uint8_t)TriggerType::EdgeAny == 6,
              "SubTriggerIsEdge is a range test: the edge triggers must stay contiguous");

// What one Bool sample means to an edge-triggered provider entry.
struct SubEdgeStep {
    bool Counted; // the level changed: the counter advances (its parity is the level)
    bool Due;     // ...and this change is one the trigger sends
};

// Classifies one sample against the previous one. EVERY level change is counted (see the header
// comment: parity is the level); the trigger only decides whether this particular change is one to
// send - a rising subscription is told about a press and not about its release, and still counts
// both, because the next press has to arrive on an odd counter.
static inline SubEdgeStep SubEdgeClassify(TriggerType Trigger, bool Now, bool Prev) {
    if (Now == Prev) return SubEdgeStep{false, false};
    const bool Due = (Trigger == TriggerType::EdgeRising)  ? Now
                   : (Trigger == TriggerType::EdgeFalling) ? !Now
                                                           : (Trigger == TriggerType::EdgeAny);
    return SubEdgeStep{true, Due};
}


#ifdef USE_SUB_REQUEST

#include <cstdio>

// Classification of an arriving counter.
enum class SubEdgeKind : uint8_t {
    Baseline = 0, // first counter since the row was installed / a re-registration was answered
    Duplicate,    // the same counter again (a verified send retried after a lost ack): ignore
    Next,         // counter + 1: the normal case, and what a wrap looks like
    Gap,          // counter + n (n >= 2): n-1 edges were never seen
    Resync,       // the counter went backwards: a restarted provider, not a gap
};

// A delta of half the ring or more is read as a provider whose counter restarted, not as a gap of
// ~32k presses: a node's provider table is RAM-only, so a reboot restarts the count at zero, and a
// counter that appears to move backwards is far more likely to be that than 32768 lost presses.
// The case is asymmetric on purpose - a real gap that large is indistinguishable from a restart,
// and replaying 32k presses into a target on successive passes would be worse than saying so.
static constexpr uint16_t SUB_EDGE_RESYNC_DELTA = 0x8000u;

// How many edge levels a row will still deliver. One per pass, so this is also the longest the
// target stays "catching up"; a larger gap is still logged with its true size (Missing), just
// capped here so the debt cannot wrap (uint8) or turn into thousands of passes.
static constexpr uint8_t SUB_EDGE_PENDING_MAX = 255;

// What the answer to an install/keepalive means for an edge row.
//
// A renewal is a plain 0401 with the SAME entry (Docs/Services/Subscriptions.md: "kept alive by
// repeating the Set Subscription command (041x/1) ... there is no separate renewal command"), and
// per the owner's ruling (2026-10-25) it may only extend the timeouts - "nothing else special
// should happen". So the answer to a row the provider has ALREADY served is a renewal: it keeps
// the counter baseline and must not write the target, because the reply carries the source's
// current level SAMPLED BEFORE it was sent, and writing that level would erase a press delivered
// in the same round trip.
//
// The FIRST answer is different: it is the install of a row that has never been served, and Docs
// CID 1 makes it the moment the row starts "from a known value" - it baselines the counter and
// writes that value. `Served()` on the lease (Core/Services/SubscriptionsLease.h) is exactly the
// "has this row ever been answered" state, which is also the ruling's "if the subscription was
// already running" test.
//
// Host-clean and named here, next to the counter accounting it belongs to, so the native test
// drives the same decision the device makes instead of a copy of it.
enum class SubEdgeAnswerKind : uint8_t { Install = 0, Renew = 1 };

static inline SubEdgeAnswerKind SubEdgeAnswer(bool Served) {
    return Served ? SubEdgeAnswerKind::Renew : SubEdgeAnswerKind::Install;
}

// One row's edge accounting. Transient: it is NOT part of the 28 B requester wire entry and NOT
// part of the 24 B `.SUBREQ` file entry (like SubRequesterLease), so every boot, recall and set
// starts it fresh - which is exactly the reset rule the counter needs.
//
// The debt is a WALK, not a retained value: `ReplayNext` is the counter whose level the next pass
// writes and `Pending` how many levels are still owed, so the last write is always the level of
// `Last` (the newest counter). That invariant is what keeps the target true: the level is parity,
// so a walk that stopped early would leave the target on the wrong side.
struct SubEdgeState {
    uint16_t Last = 0;       // the counter this row has accounted up to
    uint16_t Missing = 0;    // size of the newest gap (delta - 1); 0 unless the last arrive was a gap
    uint16_t ReplayNext = 0; // the next counter whose level the replay writes
    uint8_t Pending = 0;     // edge levels still owed to the target (saturating, one per pass)
    bool HaveBaseline = false;
    bool Report = false;     // the last arrive opened a gap/resync episode: log it (once per episode)
    bool Episode = false;    // a gap episode is open (cleared when the debt is paid or given up)

    // A fresh row (install, recall, a new set): no baseline, no debt. The next counter is taken as
    // the zero point instead of being compared against a counter from before the row existed.
    void ResetBaseline() {
        HaveBaseline = false;
        Pending = 0;
        Missing = 0;
        Report = false;
        Episode = false;
    }

    // Takes `Counter` as the new zero point: an answered (re-)registration, or a resync.
    void Rebaseline(uint16_t Counter) {
        Last = Counter;
        ReplayNext = Counter;
        HaveBaseline = true;
        Pending = 0;
        Missing = 0;
        Episode = false;
    }

    // Classifies an arriving counter and advances the state.
    //
    // `InstallAnswer` says this value is the provider's answer to an install/keepalive (the row
    // sent a Change and this is what came back): the answer re-baselines, because the provider
    // re-applies the table it was sent and its counter restarts there - a node that rebooted
    // answers with 0, and the row must take 0 as its new zero point rather than read it as a jump
    // back from whatever it had. Reset on install and on a confirmed re-registration, in one rule.
    SubEdgeKind Arrive(uint16_t Counter, bool InstallAnswer) {
        Missing = 0;
        Report = false;
        if (InstallAnswer || !HaveBaseline) {
            Rebaseline(Counter);
            return SubEdgeKind::Baseline;
        }
        // Modular on purpose: (Counter - Last) is the forward distance on the ring, so a wrap from
        // 0xFFFF to 0 is 1 - the requester never needs to know the counter wrapped.
        const uint16_t Delta = (uint16_t)(Counter - Last);
        if (Delta == 0) return SubEdgeKind::Duplicate;
        if (Delta >= SUB_EDGE_RESYNC_DELTA) {
            Missing = 0; // nothing is counted as lost: the count restarted, it did not skip
            Report = true;
            Rebaseline(Counter);
            return SubEdgeKind::Resync;
        }
        Last = Counter;
        if (Delta == 1) {
            // The normal case. A walk still catching up takes this edge as its last step (one level
            // per pass, oldest first) instead of the caller writing it out of order.
            if (Pending) {
                if (Pending < SUB_EDGE_PENDING_MAX) Pending = (uint8_t)(Pending + 1);
                else ReplayNext++; // full: drop the OLDEST owed level, the walk must still end here
            }
            return SubEdgeKind::Next;
        }
        // A gap: the whole run is owed, from the first counter that was never seen up to this one.
        // n-1 edges were never seen (Missing, for the log), and the update's own edge is the last
        // level the walk writes.
        Missing = (uint16_t)(Delta - 1);
        if (!Episode) ReplayNext = (uint16_t)(Counter - Delta + 1);
        const uint32_t Total = (uint32_t)Pending + Delta;
        if (Total > SUB_EDGE_PENDING_MAX) {
            // Capped: drop from the FRONT of the debt so the walk still ends on `Counter` and the
            // target's last level is the one this update carries.
            ReplayNext = (uint16_t)(Counter - (SUB_EDGE_PENDING_MAX - 1));
            Pending = SUB_EDGE_PENDING_MAX;
        } else {
            Pending = (uint8_t)Total;
        }
        Report = !Episode; // one line per episode, not one per lost packet
        Episode = true;
        return SubEdgeKind::Gap;
    }

    // The level the next replay writes: parity of the counter the walk has reached.
    uint8_t ReplayLevel() const { return SubEdgeLevelByte(ReplayNext); }

    // One level was written to the target (the caller does the write). Paying the last one closes
    // the episode, so a later gap logs again.
    void OnReplayed() {
        if (!Pending) return;
        ReplayNext++;
        if (--Pending == 0) Episode = false;
    }

    // The target cannot take the walk (its register no longer resolves, the field is read-only): say
    // what is being given up and close the episode. Returns the number of levels dropped.
    uint8_t GiveUp() {
        const uint8_t Lost = Pending;
        Pending = 0;
        Episode = false;
        return Lost;
    }
};

// ---------------------------------------------------------------------------
// The lines the device prints (SubscriptionsRequester.h hands them to DeviceLog). Host-side so the
// text and the argument order have one owner, like SubFormatGaveUp in SubscriptionsLease.h.
// ---------------------------------------------------------------------------
static constexpr char SUB_EDGE_TAG[] = "SUB";
static constexpr char SUB_EDGE_GAP_FMT[] =
    "TRID %04X prov %04X: gap of %u edges (counter %u -> %u), one level per pass";
static constexpr char SUB_EDGE_RESYNC_FMT[] =
    "TRID %04X prov %04X: counter went back (%u -> %u), provider restarted, baseline resynced";
static constexpr char SUB_EDGE_DROP_FMT[] =
    "TRID %04X prov %04X: replay stopped at counter %u, %u edges dropped";

// DeviceLog's own buffer (Devices/*/Log.h) is 128 bytes: the longest form of either line has to fit
// inside it, and the host test pins that it does.
static constexpr unsigned SUB_EDGE_TEXT_MAX = 128;

// Formats a gap line (returns the length snprintf produced, so Len >= Size means it was truncated).
static inline unsigned SubFormatEdgeGap(char *Dst, unsigned Size, uint16_t Trid, uint16_t ProviderAddr,
                                        unsigned From, unsigned To, unsigned Missing) {
    int Len = snprintf(Dst, Size, SUB_EDGE_GAP_FMT, (unsigned)Trid, (unsigned)ProviderAddr, Missing,
                       From, To);
    return (Len < 0) ? Size : (unsigned)Len;
}

// Formats a resync line (same contract).
static inline unsigned SubFormatEdgeResync(char *Dst, unsigned Size, uint16_t Trid, uint16_t ProviderAddr,
                                           unsigned From, unsigned To) {
    int Len = snprintf(Dst, Size, SUB_EDGE_RESYNC_FMT, (unsigned)Trid, (unsigned)ProviderAddr, From, To);
    return (Len < 0) ? Size : (unsigned)Len;
}

// Formats the line a give-up emits: the walk could not be delivered and this many edges are lost.
static inline unsigned SubFormatEdgeDrop(char *Dst, unsigned Size, uint16_t Trid, uint16_t ProviderAddr,
                                         unsigned Counter, unsigned Missing) {
    int Len = snprintf(Dst, Size, SUB_EDGE_DROP_FMT, (unsigned)Trid, (unsigned)ProviderAddr, Counter, Missing);
    return (Len < 0) ? Size : (unsigned)Len;
}

#endif // USE_SUB_REQUEST
