#pragma once

// Requester core (USE_SUB_REQUEST): the index table and triggers.
//
// Part of Core/Services/Subscriptions.h (included from there, after the class).

#include "Core/Services/SubscriptionsDefs.h"


// ===========================================================================
// Requester core (USE_SUB_REQUEST) - Tamu. TRID-sorted table; entries persisted to a file
// (1:1 copy minus the non-persistent timeout, regenerated at boot).
// ===========================================================================
#ifdef USE_SUB_REQUEST

// What makes a requester entry occupied (used by the shared table search).
inline bool SubscriptionsService::RequesterOccupied(const RequesterEntry &Entry) { return Entry.Active; }

inline RequesterEntry* SubscriptionsService::RequesterFindByTrid(uint16_t Trid) {
    return SubTableFindByTrid(RequesterTable, Trid, RequesterOccupied);
}

// The entry's provider address, re-qualified against the core's CURRENT net. The entry stores
// the address as it was net-qualified when the subscription was set (the wire form matches the
// provider's id_src); trusting that frozen net would keep sending the re-registration into the
// net the setup was written under after a net change, so the device field is re-qualified at
// every use instead.
inline uint16_t SubscriptionsService::RequesterProviderAddr(const RequesterEntry &Entry) {
    return MakeId(DeviceStatus.NetId, (uint16_t)(Entry.ProviderAddr & 0x3FF));
}

inline void SubscriptionsService::RequesterClearEntry(RequesterEntry* Entry) {
    *Entry = RequesterEntry{};
}

// Deletes `e` and compacts the table, so the active entries stay a TRID-sorted prefix
// (Docs: "stored in a sequential table, sorted by TRID").
inline void SubscriptionsService::RequesterRemove(RequesterEntry* Entry) {
    int Idx = (int)(Entry - RequesterTable);
    for (int i = Idx; i < MAX_REQUESTER_SUBS - 1; i++) RequesterTable[i] = RequesterTable[i + 1];
    RequesterClearEntry(&RequesterTable[MAX_REQUESTER_SUBS - 1]);
}

// Allocates a new active entry at its TRID-sorted position (the table stays a sorted prefix).
// Returns nullptr when the table is full. The new entry carries only its TRID.
inline RequesterEntry* SubscriptionsService::RequesterAlloc(uint16_t Trid) {
    int Count = 0;
    while (Count < MAX_REQUESTER_SUBS && RequesterTable[Count].Active) Count++;
    if (Count >= MAX_REQUESTER_SUBS) return nullptr;
    int Pos = 0;
    while (Pos < Count && RequesterTable[Pos].Trid < Trid) Pos++;
    for (int i = Count; i > Pos; i--) RequesterTable[i] = RequesterTable[i - 1];
    RequesterClearEntry(&RequesterTable[Pos]);
    RequesterTable[Pos].Trid = Trid;
    RequesterTable[Pos].Active = true;
    return &RequesterTable[Pos];
}

// Finds the entry for `trid`, or allocates it at its sorted position. Returns nullptr when the
// table is full.
inline RequesterEntry* SubscriptionsService::RequesterUpsert(uint16_t Trid) {
    RequesterEntry* Entry = RequesterFindByTrid(Trid);
    if (Entry) return Entry;
    return RequesterAlloc(Trid);
}

// Writes value bytes into the row's target register. The ONE place that decides whether a target
// can take a value at all, so the live path, an edge replay and the old non-edge path cannot drift:
//   - a 0-length value is refused before it can touch the target (see ValueUpdateApplicable);
//   - an unresolvable target is refused;
//   - a value wider than the target is cut to the target's size;
//   - the System block is not a subscription target (accepted, nothing written);
//   - the shared register setter's own refusal is passed back to the caller.
// Returns true only when the target holds the value afterwards (or is the System block).
inline bool SubscriptionsService::WriteRequesterTarget(RequesterEntry *Entry, const uint8_t *Val, uint8_t Vlen) {
    if (!ValueUpdateApplicable(Vlen)) return false;

    FieldResult Fr = SubscriptionsGetField(Entry->TargetReg);
    if (!Fr.Data) return false;
    if (Vlen > Fr.Descriptor.Size) Vlen = Fr.Descriptor.Size;

    // The System block (type 0) is not a subscription target; every other block kind is
    // written through the shared Register setter (script inputs, dynamic entries, statics).
    if (!(BlockInfoType(Entry->TargetReg) == 0 && BlockInfoInstance(Entry->TargetReg) == 0)) {
        ValueInfo Meta = Fr.Descriptor;
        Meta.Size = Vlen;
        return RegisterSetByBlockInfo(Entry->TargetReg, Meta, Val, Vlen);
    }
    return true;
}

// Applies a received value to the requester's target register (raw bytes) and confirms with
// the FNV-1a hash of the received bytes.
inline bool SubscriptionsService::ApplyRequesterValue(RequesterEntry *Entry, const uint8_t *Val, uint8_t Vlen, bool Confirm) {
    // A value update refreshes the row's 120 s deadline even when the target register is
    // momentarily unresolvable; the deadline is carried on the wire and read by the app, and keeping
    // it current is what makes it mean "when this row was last served". It is also the answer to the
    // install: the provider's CID 1 reply carries the current value, so any value from the right
    // provider restarts the retry counter (Lease.Unanswered) and puts the next renewal back on the
    // slow keepalive. Neither call removes the row - nothing on the requester side does (owner
    // ruling t_eb35360c); only an explicit cancel does.
    Entry->Lease.OnAnswer(DeviceStatus.UptimeMs);
    Entry->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);

    // A value update that carries no value bytes is refused before it can touch the target: a
    // 0-byte value would otherwise be written as-is, Size would become 0, the target would resolve
    // to no data for ever and every later update would be dropped below - the bambi f6 death, and
    // the one defect that outlived a node reboot (card t_b579fa77). Refused, the target keeps its
    // previous value.
    //
    // No status ack can bring one here: the dispatcher only routes a typed packet in the
    // Subscriptions TRID range to this service when it is NOT a FLAG_SUCCESS/FAIL reply
    // (Dispatcher.h:69-76), and an install reply always carries at least one byte. The measured
    // route is a provider whose own source is a size-0 entry, or a Register write that stored
    // Size 0 before SetEntry refused them (MemoryBlocks.h).
    //
    // The lease above still counts: the provider is alive and answering, so its retry counter is
    // reset and the row stays on the slow keepalive. There is no result code to carry either - a
    // value update is an unsolicited push, not a register request a script is waiting on - and the
    // confirmation below is deliberately not sent: it states that the value landed, so withholding
    // it leaves a confirm-required provider's hash unchanged and it retries with a real value.
    if (!WriteRequesterTarget(Entry, Val, Vlen)) return false;

    if (!Confirm) return true;

    // Confirmation: the requester sends the hash of the received value (docs CID 0: the
    // confirmation is a request, no FLAG_TYPE), at the trigger's high priority.
    uint32_t Hash = Fnv1a(Val, Vlen);
    PacketConstruct(&TxFrame, Entry->ProviderAddr, MakeService(ServiceType::Subscriptions, (uint8_t)SubCid::ValueUpdate),
                    Entry->Trid, FLAG_START | FLAG_STOP, (const uint8_t *)&Hash, sizeof(Hash),
                    PRIORITY_SUB_HIGH);
    SendAndVerifyPacket(TxFrame);
    return true;
}

// An edge-triggered update: the counter says which press this is, so a press that never arrived can
// be accounted for instead of vanishing. The classification and its baselines live in
// SubscriptionsEdge.h (host-tested); this is the device half - log once per episode, apply the
// value the update carried, and owe the missing presses to the target (the replay pass delivers
// them, one per pass, from SubscriptionsTick).
inline void SubscriptionsService::ApplyRequesterEdge(RequesterEntry *Entry, uint16_t Counter, const uint8_t *Val, uint8_t Vlen) {
    // A push that arrives while an install/keepalive is still unanswered is the first thing the row
    // has heard since it (re-)registered: whatever counter it carries is the row's zero point, not a
    // comparison. A node that rebooted and pushes before answering its install would otherwise hand
    // over a counter that looks like it went backwards (the resync case below, logged, no gap).
    const bool InstallAnswer = !Entry->Lease.Served() || Entry->Lease.Unanswered != 0;
    const uint16_t From = Entry->Edge.Last;
    const SubEdgeKind Kind = Entry->Edge.Arrive(Counter, InstallAnswer);

    // A duplicate is a verified send whose ack was lost: the press is already in the target.
    if (Kind == SubEdgeKind::Duplicate) return;

    // What a replay re-delivers. Taken before the log so the line can say whether the missing edges
    // can be delivered at all (a value too wide to keep can only be reported, not replayed).
    const bool CanReplay = Entry->Edge.Retain(Val, Vlen);

    if (Entry->Edge.Report) {
        char Line[SUB_EDGE_TEXT_MAX];
        unsigned Len = (Kind == SubEdgeKind::Resync)
            ? SubFormatEdgeResync(Line, sizeof(Line), Entry->Trid, RequesterProviderAddr(*Entry), From, Counter)
            : SubFormatEdgeGap(Line, sizeof(Line), Entry->Trid, RequesterProviderAddr(*Entry), From, Counter,
                               Entry->Edge.Missing, CanReplay);
        DeviceLog(SUB_EDGE_TAG, "%s", Line);
    }

    // The update's own value lands first (the n-th edge), then the n-1 missing ones are replayed.
    if (!ApplyRequesterValue(Entry, Val, Vlen, false)) {
        // The target refused the value, so it cannot take a replay either: say what is given up
        // (the gap line above promised a replay when the value could have been kept).
        const uint8_t Lost = Entry->Edge.GiveUp();
        if (Lost) {
            char Line[SUB_EDGE_TEXT_MAX];
            SubFormatEdgeDrop(Line, sizeof(Line), Entry->Trid, RequesterProviderAddr(*Entry), Counter, Lost);
            DeviceLog(SUB_EDGE_TAG, "%s", Line);
        }
        return;
    }
    if (!CanReplay) Entry->Edge.GiveUp(); // reported above; nothing can be delivered
}

inline void SubscriptionsService::HandleRequesterValueUpdate(const PacketFrame &Frame) {
    if (!(Frame.Flags & FLAG_TYPE)) return;
    RequesterEntry* Entry = RequesterFindByTrid(Frame.Trid);
    if (!Entry) {
        // The sender holds a subscription this device does not have: an orphan provider entry.
        // That happens when a cancel could not land (the node was offline, or the verified
        // retries ran out) and is the *only* notification we get - the node's table is its own.
        // Cancel it here, for its TRID, back to the sender. Nothing happens in the normal case,
        // and if this cancel is lost too the orphan simply announces itself again on its next
        // period, so the table converges on its own.
        PacketConstruct(&TxFrame, Frame.IdSrc, MakeService(ServiceType::Subscriptions, (uint8_t)SubCid::Change),
                        Frame.Trid, FLAG_START | FLAG_STOP, nullptr, 0);
        SendAndVerifyPacket(TxFrame);
        return;
    }
    const uint16_t Len = PayloadBytes(Frame);

    // An edge-triggered row sees two payload shapes, and they cannot collide because a value is at
    // least one byte (SubscriptionsEdge.h):
    //   >= 3 bytes - a PUSH: the provider's edge counter (2) and the value behind it;
    //   <= 2 bytes - the provider's ANSWER to "Change subscription" (Docs: "Current value"), which
    //                carries the value alone. That is the row's answer, not a press, so it re-baselines
    //                the counter instead of being classified: the next push becomes the zero point.
    if (SubTriggerIsEdge(Entry->Sub.Trigger)) {
        if (Len >= SUB_EDGE_COUNTER_SIZE + 1) {
            // The counter is consumed here and never reaches the target: the apply below is handed
            // the value bytes only, at their own offset and length.
            ApplyRequesterEdge(Entry, SubEdgeCounterLoad(Frame.Payload), Frame.Payload + SUB_EDGE_COUNTER_SIZE,
                               (uint8_t)(Len - SUB_EDGE_COUNTER_SIZE));
        } else {
            Entry->Edge.ResetBaseline();
            ApplyRequesterValue(Entry, Frame.Payload, (uint8_t)Len, false);
        }
        return;
    }

    // Docs: the value update is "sent as response packet, request is confirmation IF NEEDED" -
    // only OnChangeConfirm repeats until confirmed. Confirming every trigger would also
    // overwrite the provider's Hash/Hashlike state, which the delta trigger uses as its last
    // sent scalar value.
    const bool Confirm = Entry->Sub.Trigger == TriggerType::OnChangeConfirm;
    ApplyRequesterValue(Entry, Frame.Payload, (uint8_t)Len, Confirm);
}

// Delivers the edges a gap lost, one per tick (Docs: the requester does not drop a "subscription
// happened", it catches its target up). One per pass on purpose: the target is written once per
// main-loop pass, so a script counting presses behind it sees each recovered press as a separate
// pass instead of a burst it cannot count.
//
// A target that cannot take a replay - the value was too wide to retain, or the write is refused
// (the register no longer resolves, the field is read-only) - is reported and the remaining debt is
// given up: replaying a press into a target that will not take it would retry for ever.
inline void SubscriptionsService::SubscriptionsReplayPending() {
    for (int n = 0; n < MAX_REQUESTER_SUBS; n++) {
        if (++ReplayCursor >= MAX_REQUESTER_SUBS) ReplayCursor = 0;
        RequesterEntry* Entry = &RequesterTable[ReplayCursor];
        if (!Entry->Active || Entry->Edge.Pending == 0) continue;
        if (WriteRequesterTarget(Entry, Entry->Edge.Value, Entry->Edge.ValueLen)) {
            Entry->Edge.OnReplayed();
            return;
        }
        const uint8_t Lost = Entry->Edge.GiveUp();
        char Line[SUB_EDGE_TEXT_MAX];
        SubFormatEdgeDrop(Line, sizeof(Line), Entry->Trid, RequesterProviderAddr(*Entry),
                          Entry->Edge.Last, Lost);
        DeviceLog(SUB_EDGE_TAG, "%s", Line);
        return;
    }
}
#endif // USE_SUB_REQUEST
