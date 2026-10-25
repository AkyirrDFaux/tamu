#pragma once

// The edge counter an edge-triggered value update carries, and what the requester does with it.
//
// An edge-triggered subscription is a *press*: the value is a Bool and the update exists to say
// "this happened". The wire used to carry the value alone, which made a press that arrived and a
// press that was lost look identical at the requester - the target already held the value, so a
// lost update changed nothing and said nothing (the "switch dies at random" family). The provider
// has always counted its edges (ProviderEntry::Hash, Docs "Counter: for Bool edges only"); it now
// sends that counter ahead of the value, so a lost update is a *visible* loss:
//
//   value update payload, edge triggers only (Docs/Services/Subscriptions.md, "Value update"):
//
//     +--------+--------+-----------------+
//     | ctr lo | ctr hi | value bytes ... |
//     +--------+--------+-----------------+
//       0        1        2 ..
//
//   - u16, little-endian: the low 16 bits of the provider's 32-bit edge counter. The provider
//     increments the counter once per detected edge, so the wire value is `counter mod 2^16`.
//   - the payload length is SUB_EDGE_COUNTER_SIZE + the value length; for the documented Bool edge
//     that is 3 bytes.
//   - every other trigger keeps its payload byte-for-byte: counter-free, value from byte 0.
//   - the ANSWER to "Change subscription" (0401) is unchanged and carries the current value alone
//     (Docs CID 1, "Current value"). A value is at least one byte, so the two shapes cannot
//     collide: >= 3 bytes is a push with the counter in front, <= 2 bytes is the answer. The answer
//     is not a press - it tells the row to forget its baseline, and the next push is the zero point.
//     Leaving it alone is also what makes the feature fit the DAS, which has ~68 B of code space
//     free: carrying the counter on the answer too cost ~120 B there.
//
// Why both ends must agree on the encoding: a mismatch (byte order, offset, width) does not fail
// loudly - every update just looks like a gap of a random size - so the encoding has exactly one
// owner (SubEdgeCounterStore/Load below) and both sides call it. The two images must be flashed
// together for the same reason: an old requester would write the counter's bytes into the target as
// if they were the value, and an old provider's counter-free push cannot be told from an answer.
//
// Host-clean (no device headers, no globals) so test/native/edge_counter_test.cpp drives the same
// state machine the core runs: RequesterEntry embeds SubEdgeState and the requester only calls its
// methods (Core/Services/SubscriptionsRequester.h). The requester half is behind USE_SUB_REQUEST,
// the flag that compiles the requester role at all - the DAS (provider only) pays nothing for it.

#include <cstdint>
#include <cstring>

#include "Core/Types/Enums.h"

// The counter ahead of an edge value: 16 bits, so an edge update costs two bytes of payload on a
// high-priority packet that can be sent several times a second.
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

// Is this trigger an edge trigger (the only kind that carries the counter)?
// A range test, not three equalities: it is called at every provider send and on both ends, and the
// enum's edge values are contiguous (pinned below).
static inline bool SubTriggerIsEdge(TriggerType Trigger) {
    const uint8_t Value = (uint8_t)Trigger;
    return Value >= (uint8_t)TriggerType::EdgeRising && Value <= (uint8_t)TriggerType::EdgeAny;
}
static_assert((uint8_t)TriggerType::EdgeRising == 4 && (uint8_t)TriggerType::EdgeFalling == 5 &&
                  (uint8_t)TriggerType::EdgeAny == 6,
              "SubTriggerIsEdge is a range test: the edge triggers must stay contiguous");


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

// How many missing edges a row will replay. One per pass, so this is also the longest the target
// stays "catching up"; a larger gap is still logged with its true size (Missing), just capped here
// so the debt cannot wrap (uint8) or turn into thousands of passes.
static constexpr uint8_t SUB_EDGE_PENDING_MAX = 255;

// The value bytes kept for a replay. Edge triggers are Bool-only in the docs, so 1 byte covers
// every documented case; the margin is for a provider entry that set an edge trigger on something
// wider. A wider value still updates the target - it just cannot be replayed, and the gap is
// logged as dropped instead.
static constexpr uint8_t SUB_EDGE_REPLAY_VALUE_MAX = 4;

// One row's edge accounting. Transient: it is NOT part of the 28 B requester wire entry and NOT
// part of the 24 B `.SUBREQ` file entry (like SubRequesterLease), so every boot, recall and set
// starts it fresh - which is exactly the reset rule the counter needs.
struct SubEdgeState {
    uint16_t Last = 0;      // the counter this row has accounted up to
    uint16_t Missing = 0;   // size of the newest gap (delta - 1); 0 unless the last arrive was a gap
    uint8_t Pending = 0;    // missing edges still owed to the target (saturating, one per pass)
    bool HaveBaseline = false;
    bool Report = false;    // the last arrive opened a gap/resync episode: log it (once per episode)
    bool Episode = false;   // a gap episode is open (cleared when the debt is paid or given up)
    uint8_t ValueLen = 0;   // retained value bytes (0 = nothing to replay with)
    uint8_t Value[SUB_EDGE_REPLAY_VALUE_MAX] = {};

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
        if (Delta == 1) return SubEdgeKind::Next;
        // n-1 edges were never seen: the update itself delivers the n-th.
        Missing = (uint16_t)(Delta - 1);
        const uint32_t Total = (uint32_t)Pending + Missing;
        Pending = (uint8_t)(Total > SUB_EDGE_PENDING_MAX ? SUB_EDGE_PENDING_MAX : Total);
        Report = !Episode; // one line per episode, not one per lost packet
        Episode = true;
        return SubEdgeKind::Gap;
    }

    // Keeps the value bytes a replay re-delivers. False when they cannot be kept (no value, or one
    // wider than the buffer): the gap can then only be logged, never delivered.
    bool Retain(const uint8_t *Val, uint8_t Vlen) {
        if (Vlen == 0 || Vlen > SUB_EDGE_REPLAY_VALUE_MAX || !Val) {
            ValueLen = 0;
            return false;
        }
        memcpy(Value, Val, Vlen);
        ValueLen = Vlen;
        return true;
    }

    bool CanReplay() const { return ValueLen != 0; }

    // One replay was delivered to the target (the caller does the write). Paying the last one closes
    // the episode, so a later gap logs again.
    void OnReplayed() {
        if (Pending) Pending--;
        if (!Pending) Episode = false;
    }

    // A replay cannot be delivered (no value was retained, or the target refused the write): say
    // what is being given up and close the episode. Returns the number of edges dropped.
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
    "TRID %04X prov %04X: gap of %u edges (counter %u -> %u), %s";
static constexpr char SUB_EDGE_GAP_REPLAY[] = "replaying";
static constexpr char SUB_EDGE_GAP_DROPPED[] = "target cannot take a replay, dropped";
static constexpr char SUB_EDGE_RESYNC_FMT[] =
    "TRID %04X prov %04X: counter went back (%u -> %u), provider restarted, baseline resynced";
static constexpr char SUB_EDGE_DROP_FMT[] =
    "TRID %04X prov %04X: replay stopped at counter %u, %u edges dropped";

// DeviceLog's own buffer (Devices/*/Log.h) is 128 bytes: the longest form of either line has to fit
// inside it, and the host test pins that it does.
static constexpr unsigned SUB_EDGE_TEXT_MAX = 128;

// Formats a gap line (returns the length snprintf produced, so Len >= Size means it was truncated).
static inline unsigned SubFormatEdgeGap(char *Dst, unsigned Size, uint16_t Trid, uint16_t ProviderAddr,
                                        unsigned From, unsigned To, unsigned Missing, bool Replaying) {
    int Len = snprintf(Dst, Size, SUB_EDGE_GAP_FMT, (unsigned)Trid, (unsigned)ProviderAddr, Missing,
                       From, To, Replaying ? SUB_EDGE_GAP_REPLAY : SUB_EDGE_GAP_DROPPED);
    return (Len < 0) ? Size : (unsigned)Len;
}

// Formats a resync line (same contract).
static inline unsigned SubFormatEdgeResync(char *Dst, unsigned Size, uint16_t Trid, uint16_t ProviderAddr,
                                           unsigned From, unsigned To) {
    int Len = snprintf(Dst, Size, SUB_EDGE_RESYNC_FMT, (unsigned)Trid, (unsigned)ProviderAddr, From, To);
    return (Len < 0) ? Size : (unsigned)Len;
}

// Formats the line a give-up emits: the replay could not be delivered and this many edges are lost.
static inline unsigned SubFormatEdgeDrop(char *Dst, unsigned Size, uint16_t Trid, uint16_t ProviderAddr,
                                         unsigned Counter, unsigned Missing) {
    int Len = snprintf(Dst, Size, SUB_EDGE_DROP_FMT, (unsigned)Trid, (unsigned)ProviderAddr, Counter, Missing);
    return (Len < 0) ? Size : (unsigned)Len;
}

#endif // USE_SUB_REQUEST
