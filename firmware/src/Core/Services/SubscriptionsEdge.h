#pragma once

// The edge counter an edge-triggered value update carries, and what the requester does with it.
//
// An edge-triggered subscription is a *press*: the value is a Bool and the update exists to say
// "this happened". The wire used to carry the value alone, which made a press that arrived and a
// press that was lost look identical at the requester - the target already held the value, so a
// lost update changed nothing and said nothing (the "switch dies at random" family). The provider
// has always counted its edges (ProviderEntry::Hash, Docs "Counter: for Bool edges only"); the
// counter is now the update's whole payload:
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
//   - the counter advances once per level change, so the advance an update carries (`Delta`) is how
//     many changes happened since the row's last counter; SubEdgeDirectionCount below says how many
//     of those are edges of the trigger's own direction.
//   - the payload length is exactly SUB_EDGE_COUNTER_SIZE. Nothing is appended.
//   - every other trigger keeps its payload byte-for-byte: counter-free, value from byte 0.
//   - the ANSWER to "Change subscription" (0401) is unchanged and carries the current value alone
//     (Docs CID 1, "Current value"); for an edge row that is its Bool, one byte. A value is at
//     least one byte and a push is exactly two, so the shapes cannot collide. The answer is not a
//     press, and what it may do depends on whether the row has run before (SubEdgeAnswer below): the
//     FIRST answer is the install, so it starts the row from that value and makes the next push the
//     zero point, while the answer to a RENEWAL only extends the deadlines - it keeps the baseline
//     (the provider keeps its counter across renewals) and does not write the target (that level was
//     sampled before the request, so writing it would undo a press from the same round trip).
//
// THE TARGET IS A PULSE, NOT A LEVEL (owner ruling, 2026-10-27). The counter and the target value are
// two different things and they stay separate: the counter is the provider's bookkeeping, its parity
// is the level (odd = pressed, even = lifted), and it is only READ here - to learn how many edges are
// new. What the requester WRITES is the pulse. It used to write that parity, i.e. the level, and never
// clear it, so a button pressed once read true for ever and every consumer had to remember the last
// level to see a second press. Now an edge update sets a BUDGET of n loop passes, n being the number
// of edges of the trigger's direction that update carries, and every requester loop pass writes TRUE
// and spends one; the pass after the last one writes FALSE once, and the target then stays false until
// the next edge:
//
//   one press                  -> T F . . . . .   one loop of true, then false
//   3 edges in one update      -> T T T F . . .   three loops, then false
//   held button (one edge)     -> T F . . . . .   one loop, never a level that sits there
//   no edge at all             -> . . . . . . .   nothing written, the field stays false
//
// The true loops are CONSECUTIVE on purpose (owner clarification): one loop per edge, no spacing, no
// gap - the budget simply decrements once per pass. So a plain consumer that reads the field once
// per loop and counts the loops it was true counts presses, without keeping the previous level
// itself; that memory is exactly what would turn a burst of three edges into one step.
//
// Why the provider counts BOTH directions, whatever the trigger: parity can only mean the level if
// every level change moves the counter. A rising-only counter would stay odd after the release and
// the requester could not tell a press from a held switch. So the counter counts level changes and the
// trigger only decides which of them are SENT (SubEdgeClassify below) - and which of them, on
// arrival, are edges of the subscribed direction (SubEdgeDirectionCount).
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
// both, because the counter is what makes the next press tell its distance from the last one.
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
    Gap,          // counter + n (n >= 2): n-1 counters were never seen, some edges were lost
    Resync,       // the counter went backwards: a restarted provider, not a gap
};

// A delta of half the ring or more is read as a provider whose counter restarted, not as a gap of
// ~32k presses: a node's provider table is RAM-only, so a reboot restarts the count at zero, and a
// counter that appears to move backwards is far more likely to be that than 32768 lost presses.
// The case is asymmetric on purpose - a real gap that large is indistinguishable from a restart,
// and pulsing 32k times into a target on successive passes would be worse than saying so.
static constexpr uint16_t SUB_EDGE_RESYNC_DELTA = 0x8000u;

// How many true passes a row will still deliver from one update. One pass per edge, so this is also
// the longest a single pulse can last; a larger burst is still logged with its true size (Missing),
// just capped here so the budget cannot wrap (uint8) or turn into thousands of passes.
static constexpr uint8_t SUB_EDGE_PULSE_MAX = 255;

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

// How many edges of the trigger's DIRECTION a counter advance of `Delta` (from counter `From`)
// contains - the number of true passes that advance is worth.
//
// The counter counts every level change, both directions, and the trigger decided which changes were
// sent. Parity is the level, so for a rising subscription two counter steps are one press (the press
// that was sent and the release that was not), and only every other counter step is an edge of the
// subscribed direction:
//
//   EdgeRising  from an odd counter (pressed) -> odd to odd:   delta/2      (16 -> 20: two presses)
//   EdgeRising  from an even counter (lifted) -> even to odd:  (delta+1)/2  (16 -> 19: two presses)
//   EdgeFalling the mirror image of the above
//   EdgeAny     every step is an edge of the direction: delta
//
// The parity is taken from `From` rather than assumed, so a provider whose pushes are one step out of
// phase still yields the right count instead of half or double. A release that somehow arrives on a
// rising subscription is a step that lands on an even counter: it is not an edge of that direction
// and is worth no pulse (it is not a press), and the same goes for a press on a falling one.
static inline uint16_t SubEdgeDirectionCount(TriggerType Trigger, uint16_t From, uint16_t Delta) {
    if (Trigger == TriggerType::EdgeAny) return Delta;
    // A press lands on an odd counter, a lift on an even one.
    const bool WantOdd = (Trigger == TriggerType::EdgeRising);
    const bool FromOdd = SubEdgeLevel(From);
    // The steps that land on the wanted parity: those with an odd offset, or with an even one.
    const bool OddOffsets = (FromOdd != WantOdd);
    return OddOffsets ? (uint16_t)((Delta + 1u) / 2u) : (uint16_t)(Delta / 2u);
}

// One row's edge accounting. Transient: it is NOT part of the 28 B requester wire entry and NOT
// part of the 24 B `.SUBREQ` file entry (like SubRequesterLease), so every boot, recall and set
// starts it fresh - which is exactly the reset rule the counter needs.
//
// The target's obligation is a BUDGET of true passes, never a retained level: `Arrive` adds the
// edges an update carried, every requester loop pass spends one (NextPass), and the pass after the
// last one writes the false that closes the pulse. Nothing here holds "the last level", so a burst of
// n edges cannot collapse into one step and a button held down cannot leave the target true.
struct SubEdgeState {
    uint16_t Last = 0;    // the counter this row has accounted up to
    uint16_t Missing = 0; // counters the newest gap skipped (delta - 1); 0 unless the last arrive was a gap
    uint8_t Budget = 0;   // true passes still owed to the target (saturating, one per pass)
    // The target is (or may be) true, so the next idle pass has to close the pulse with one false.
    // A fresh row starts true: the requester does not know what the target holds after a boot, a
    // recall or a set (the install may have written a level), and the ruling's "no edge -> false"
    // has to hold from the first pass rather than from the first press.
    bool High = true;
    bool HaveBaseline = false;
    bool Report = false; // the last arrive opened a gap: log it

    // A fresh row (install, recall, a new set): no baseline, no debt. The next counter is taken as
    // the zero point instead of being compared against a counter from before the row existed.
    void ResetBaseline() {
        HaveBaseline = false;
        Budget = 0;
        Missing = 0;
        Report = false;
        High = true;
    }

    // Takes `Counter` as the new zero point: an answered (re-)registration, or a resync. The pulse
    // budget is deliberately NOT cleared here: a resync says the provider's count restarted, not that
    // the edges already owed to the target never happened.
    void Rebaseline(uint16_t Counter) {
        Last = Counter;
        HaveBaseline = true;
        Missing = 0;
        Report = false;
    }

    // Classifies an arriving counter, advances the state, and adds the true passes the target is owed.
    //
    // `InstallAnswer` says this value is the provider's answer to the row's FIRST Change - the row
    // has never been served, so this is the install and its counter is the zero point rather than a
    // comparison. A node that rebooted (RAM-only provider table) answers its install with 0, and the
    // row must take 0 as its new zero point instead of reading it as a jump back from whatever it
    // had. A push that crosses a RENEWAL is NOT an install any more: the provider keeps its counter
    // across renewals (owner ruling, 2026-10-25, SubscriptionsProviderReset.h), so that push is an
    // ordinary press and has to be compared - and a provider that really did restart sends a counter
    // that went backwards, which the resync case below already handles.
    SubEdgeKind Arrive(TriggerType Trigger, uint16_t Counter, bool InstallAnswer) {
        const uint16_t From = Last;
        Missing = 0;
        Report = false;
        if (InstallAnswer || !HaveBaseline) {
            Rebaseline(Counter);
            // A push exists because an edge of the subscribed direction existed - the provider sends
            // nothing otherwise. The zero point cannot say how many came before it, so this one is
            // worth exactly the edge that carried it: one true pass, not none (a first press after a
            // boot or a set must still reach the consumer) and not the level it happens to sit on.
            AddPulses(1);
            return SubEdgeKind::Baseline;
        }
        // Modular on purpose: (Counter - Last) is the forward distance on the ring, so a wrap from
        // 0xFFFF to 0 is 1 - the requester never needs to know the counter wrapped.
        const uint16_t Advance = (uint16_t)(Counter - From);
        if (Advance == 0) return SubEdgeKind::Duplicate;
        if (Advance >= SUB_EDGE_RESYNC_DELTA) {
            // The count restarted: nothing is owed for it, and what is already owed stays owed.
            Rebaseline(Counter);
            Report = true;
            return SubEdgeKind::Resync;
        }
        Last = Counter;
        Missing = (Advance > 1) ? (uint16_t)(Advance - 1) : 0;
        Report = (Advance > 1); // one line per gap, not one per lost packet
        // The pulses: the edges of the trigger's direction that this advance contains. It extends a
        // pulse that is still running rather than replacing it (see AddPulses).
        AddPulses(SubEdgeDirectionCount(Trigger, From, Advance));
        return (Advance == 1) ? SubEdgeKind::Next : SubEdgeKind::Gap;
    }

    // Extends the budget by `Count` more true passes. An edge that arrives before the budget is
    // exhausted does not restart or replace the pulse: it ADDS its own edges, so nothing already
    // promised to the consumer is lost - the target stays true for (passes left + Count) more passes
    // and then false. Saturating at SUB_EDGE_PULSE_MAX: a burst beyond it is reported by its log line
    // with its true size and the beyond-cap edges are dropped rather than wrapped into a short pulse.
    void AddPulses(uint16_t Count) {
        const uint32_t Total = (uint32_t)Budget + (uint32_t)Count;
        Budget = (Total > SUB_EDGE_PULSE_MAX) ? (uint8_t)SUB_EDGE_PULSE_MAX : (uint8_t)Total;
    }

    // One requester loop pass: the value to write to the target, if any.
    //   budget left      -> true, one pass spent   (consecutive: one loop per edge)
    //   budget just ended-> one false, which closes the pulse
    //   idle and false   -> nothing to write, the field already is false
    // So n edges are n true passes and the false that follows them. An idle row is silent, but only
    // from its second pass on: a fresh row (install, recall, a new set, a boot) starts with High set,
    // so its FIRST idle pass writes one clearing false, and every pass after that writes nothing.
    struct SubPulseStep {
        bool Write;
        uint8_t Value;
    };

    SubPulseStep NextPass() {
        if (Budget) {
            Budget--;
            High = true;
            return SubPulseStep{true, 1};
        }
        if (!High) return SubPulseStep{false, 0};
        High = false;
        return SubPulseStep{true, 0};
    }

    // The target cannot take the pulse (its register no longer resolves, the field is read-only):
    // give up what is left and say how many edges are being dropped. A target that refused one write
    // refuses the rest, so nothing is retried - the row idles until the next edge re-arms it.
    uint8_t GiveUp() {
        const uint8_t Lost = Budget;
        Budget = 0;
        High = false;
        return Lost;
    }
};

// ---------------------------------------------------------------------------
// The lines the device prints (SubscriptionsRequester.h hands them to DeviceLog). Host-side so the
// text and the argument order have one owner, like SubFormatGaveUp in SubscriptionsLease.h.
// ---------------------------------------------------------------------------
static constexpr char SUB_EDGE_TAG[] = "SUB";
static constexpr char SUB_EDGE_GAP_FMT[] =
    "TRID %04X prov %04X: gap of %u edges (counter %u -> %u), one pulse per pass";
static constexpr char SUB_EDGE_RESYNC_FMT[] =
    "TRID %04X prov %04X: counter went back (%u -> %u), provider restarted, baseline resynced";
static constexpr char SUB_EDGE_DROP_FMT[] =
    "TRID %04X prov %04X: target refused the pulse, %u edges dropped";

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

// Formats the line a give-up emits: the target would not take the pulse and this many edges are lost.
static inline unsigned SubFormatEdgeDrop(char *Dst, unsigned Size, uint16_t Trid, uint16_t ProviderAddr,
                                         unsigned Missing) {
    int Len = snprintf(Dst, Size, SUB_EDGE_DROP_FMT, (unsigned)Trid, (unsigned)ProviderAddr, Missing);
    return (Len < 0) ? Size : (unsigned)Len;
}

#endif // USE_SUB_REQUEST
