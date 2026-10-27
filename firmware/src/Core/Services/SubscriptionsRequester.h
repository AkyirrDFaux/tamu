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

    // The row's Hash/Hashlike records what it has SEEN (Docs "Requester table entry": the counter
    // for an edge trigger, the last value otherwise), and nothing is decided with it here - the
    // requester writes the value it is handed, so this is bookkeeping the app can read off *Get
    // subscriptions*, never a send gate and never something written into the target.
    //
    // A value that fits the 32-bit field IS the last value, little-endian and zero-extended (Bool,
    // Index, Uint32 and Number all do - the types the docs name for "last value"); a wider value
    // cannot be held, so it is kept as its FNV-1a fingerprint, the same form the provider's OnChange
    // reference uses. An EDGE row's field is a counter, not a value, and is written by
    // ApplyRequesterEdge alone: this path only ever hands an edge row the install's level (the 0401
    // answer), which carries no counter and must not overwrite one.
    if (!SubTriggerIsEdge(Entry->Sub.Trigger)) {
        if (Vlen <= sizeof(Entry->Hash)) {
            uint32_t Seen = 0;
            for (uint8_t i = 0; i < Vlen; i++) Seen |= (uint32_t)Val[i] << (8 * i);
            Entry->Hash = Seen;
        } else {
            Entry->Hash = Fnv1a(Val, Vlen);
        }
    }

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

// An edge-triggered update: the counter IS the payload and says how many edges are new; the target is
// owed one TRUE pass per new edge of the trigger's direction and one FALSE after them (owner ruling,
// 2026-10-27). The classification and the pulse accounting live in SubscriptionsEdge.h (host-tested);
// this is the device half - log once per gap, and let Arrive add the passes to the row's budget. The
// target itself is written by SubscriptionsPulseTargets, one pass at a time, and never here.
inline void SubscriptionsService::ApplyRequesterEdge(RequesterEntry *Entry, uint16_t Counter) {
    // A push that arrives while an install is still unanswered is the first thing a row that has
    // NEVER been served has heard since it was set: whatever counter it carries is the row's zero
    // point, not a comparison. That is the install case only. A push that crosses a RENEWAL is not
    // a re-registration any more: the provider keeps its counter across renewals (owner ruling,
    // 2026-10-25), so its counter is continuous and the push is an ordinary press that has to be
    // compared - treating it as a zero point would throw away the presses counted since the last
    // one. A provider that really did restart (a node reboot clears its RAM-only table) sends a
    // counter that went BACKWARDS, which `Arrive` already handles as a resync below.
    const bool InstallAnswer = !Entry->Lease.Served();
    const uint16_t From = Entry->Edge.Last;
    // Arrive classifies the counter AND adds the true passes this update is worth (one per edge of the
    // trigger's direction), so the device cannot disagree with the host test about either.
    const SubEdgeKind Kind = Entry->Edge.Arrive(Entry->Sub.Trigger, Counter, InstallAnswer);

    // The row's Hash/Hashlike is the wire's copy of the counter this row has accounted up to, so the
    // two can never disagree: it is taken from the state machine AFTER the classification instead of
    // from `Counter`, and a duplicate (a retried send whose ack was lost) therefore leaves it where
    // it was. An install, a recall or a reboot starts it at 0 - the field is not persisted (owner
    // ruling, 2026-10-27) - and the next push after that is the row's zero point, so the gaps the
    // device missed while it was off are NOT replayed as pulses on the first update.
    Entry->Hash = Entry->Edge.Last;

    // Every push is an answer from the provider, and keeping that bookkeeping current is what makes
    // the 120 s deadline mean "when this row was last served" (see ApplyRequesterValue). It is done
    // here as well because a gap only arms the pulse - its budget is spent one pass at a time - and
    // the lease must not depend on whether this update had edges to deliver.
    Entry->Lease.OnAnswer(DeviceStatus.UptimeMs);
    Entry->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);

    // A duplicate is a verified send whose ack was lost: those edges are already in the budget.
    if (Kind == SubEdgeKind::Duplicate) return;

    if (Entry->Edge.Report) {
        char Line[SUB_EDGE_TEXT_MAX];
        if (Kind == SubEdgeKind::Resync)
            SubFormatEdgeResync(Line, sizeof(Line), Entry->Trid, RequesterProviderAddr(*Entry), From, Counter);
        else
            SubFormatEdgeGap(Line, sizeof(Line), Entry->Trid, RequesterProviderAddr(*Entry), From, Counter,
                             Entry->Edge.Missing);
        DeviceLog(SUB_EDGE_TAG, "%s", Line);
    }

    // What may reach the target: nothing, here. The update has armed the row's pulse budget (Arrive),
    // and the budget is spent one TRUE pass at a time by SubscriptionsPulseTargets - writing anything
    // from this update would put a value in front of the passes it still owes, and a value is not what
    // an edge target carries any more. The counter itself is never written anywhere.
}

// The requester's acknowledgement of an edge update: the counter it received, back to the provider as
// the confirmation word (SubEdgeConfirmWord - the counter in both halves), which marks exactly that
// counter confirmed and stops the re-send (SubscriptionsEdge.h, SubEdgeAnswers; the provider half is
// HandleProviderConfirmation). Four bytes, the same shape the value confirmation carries (for an edge
// row the counter IS the Hash/Hashlike a CID 0 confirmation holds, Docs/Services/Subscriptions.md),
// and it carries WHICH counter is being answered: an answer that arrives after a newer edge counted
// must not confirm the newer one.
//
// It is a request, not a response (no FLAG_TYPE), at the trigger's high priority - the same shape
// the confirm trigger's confirmation has.
inline void SubscriptionsService::SubscriptionsConfirmEdge(RequesterEntry *Entry, uint16_t Counter) {
    const uint32_t Answer = SubEdgeConfirmWord(Counter);
    PacketConstruct(&TxFrame, Entry->ProviderAddr,
                    MakeService(ServiceType::Subscriptions, (uint8_t)SubCid::ValueUpdate), Entry->Trid,
                    FLAG_START | FLAG_STOP, (const uint8_t *)&Answer, sizeof(Answer), PRIORITY_SUB_HIGH);
    SendAndVerifyPacket(TxFrame);
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
    // least one byte and a push is exactly two (SubscriptionsEdge.h):
    //   >= 2 bytes - a PUSH: the provider's edge counter (u16, the whole payload). The counter is
    //                consumed here and never reaches the target; it only says how many edges are new.
    //   <  2 bytes - the provider's ANSWER to "Change subscription" (Docs: "Current value"), which
    //                carries the value alone (for an edge row, its Bool). What that answer MAY do
    //                depends on whether the row has been served before (SubEdgeAnswer): the first
    //                one is the install, any later one is a renewal and extends the deadlines only.
    if (SubTriggerIsEdge(Entry->Sub.Trigger)) {
        if (Len >= SUB_EDGE_COUNTER_SIZE) {
            const uint16_t Counter = SubEdgeCounterLoad(Frame.Payload);
            ApplyRequesterEdge(Entry, Counter);
            // An edge update is CONFIRMED too, when the provider asks for it (owner ruling,
            // 2026-10-27): the ack echoes the counter it answers, so the provider can mark exactly
            // that counter confirmed and stop re-sending it (SubscriptionsEdge.h, SubEdgeConfirm).
            // It is sent for a DUPLICATE as well - a duplicate is a re-send whose earlier ack was
            // lost, so staying silent would keep the provider repeating for ever. Gated on
            // FLAG_REQACK like every other confirmation: a provider that does not expect a reply
            // gets none.
            if (Frame.Flags & FLAG_REQACK) SubscriptionsConfirmEdge(Entry, Counter);
        } else if (SubEdgeAnswer(Entry->Lease.Served()) == SubEdgeAnswerKind::Install) {
            Entry->Edge.ResetBaseline();
            ApplyRequesterValue(Entry, Frame.Payload, (uint8_t)Len, false);
        } else {
            // A renewal: a plain 0401 with the same entry, so it may only extend the row's
            // deadlines (owner ruling, 2026-10-25 - "nothing else special should happen"). The
            // baseline is KEPT: the provider no longer restarts its counter on a renewal
            // (SubscriptionsProvider.h), so the next push is compared against the row's last
            // counter instead of being taken as a new zero point. The target is NOT written: this
            // reply carries the level the source had when the request was made, so writing it
            // would undo a press that landed while the reply was in flight.
            Entry->Lease.OnAnswer(DeviceStatus.UptimeMs);
            Entry->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
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

// One requester loop pass over the edge targets: this is what makes an edge target a PULSE instead of
// a held level (owner ruling, 2026-10-27). SubEdgeState::NextPass hands out `true` while the row's
// budget lasts - one pass per edge of the trigger's direction that arrived - and the single `false`
// that closes the pulse after the last one, so a plain consumer that reads the field once per pass
// counts presses without remembering the previous level. Nothing is written while a row has no pulse,
// so an untouched button costs no register writes at all.
//
// A target that cannot take a pulse - the register no longer resolves, the field is read-only - is
// reported with the number of edges being dropped and the row idles until the next edge re-arms it;
// retrying a target that refuses every write would spin for ever.
inline void SubscriptionsService::SubscriptionsPulseTargets() {
    for (int n = 0; n < MAX_REQUESTER_SUBS; n++) {
        RequesterEntry *Entry = &RequesterTable[n];
        if (!Entry->Active || !SubTriggerIsEdge(Entry->Sub.Trigger)) continue;
        const SubEdgeState::SubPulseStep Step = Entry->Edge.NextPass();
        if (!Step.Write) continue;
        // One byte: the pulse value, never the counter as a number.
        const uint8_t Value = Step.Value;
        if (WriteRequesterTarget(Entry, &Value, 1)) continue;
        const uint8_t Lost = Entry->Edge.GiveUp();
        if (Lost == 0) continue;
        char Line[SUB_EDGE_TEXT_MAX];
        SubFormatEdgeDrop(Line, sizeof(Line), Entry->Trid, RequesterProviderAddr(*Entry), Lost);
        DeviceLog(SUB_EDGE_TAG, "%s", Line);
    }
}
#endif // USE_SUB_REQUEST
