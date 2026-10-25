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

// Applies a received value to the requester's target register (raw bytes) and confirms with
// the FNV-1a hash of the received bytes.
inline void SubscriptionsService::ApplyRequesterValue(RequesterEntry *Entry, const uint8_t *Val, uint8_t Vlen, bool Confirm) {
    // A value update renews the 120 s lease even when the target register is momentarily
    // unresolvable; otherwise a briefly unavailable target would let the entry expire and
    // cancel its still-live provider. It is also the answer to the install: the provider's CID 1
    // reply carries the current value, so any value from the right provider clears the entry's
    // unanswered count and puts its next renewal back on the slow keepalive.
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
    // The lease above still counts: the provider is alive and answering, so the entry must not
    // expire and cancel it. There is no result code to carry either - a value update is an
    // unsolicited push, not a register request a script is waiting on - and the confirmation below
    // is deliberately not sent: it states that the value landed, so withholding it leaves a
    // confirm-required provider's hash unchanged and it retries with a real value.
    if (!ValueUpdateApplicable(Vlen)) return;

    FieldResult Fr = SubscriptionsGetField(Entry->TargetReg);
    if (!Fr.Data) return;
    if (Vlen > Fr.Descriptor.Size) Vlen = Fr.Descriptor.Size;

    // The System block (type 0) is not a subscription target; every other block kind is
    // written through the shared Register setter (script inputs, dynamic entries, statics).
    if (!(BlockInfoType(Entry->TargetReg) == 0 && BlockInfoInstance(Entry->TargetReg) == 0)) {
        ValueInfo Meta = Fr.Descriptor;
        Meta.Size = Vlen;
        RegisterSetByBlockInfo(Entry->TargetReg, Meta, Val, Vlen);
    }

    if (!Confirm) return;

    // Confirmation: the requester sends the hash of the received value (docs CID 0: the
    // confirmation is a request, no FLAG_TYPE), at the trigger's high priority.
    uint32_t Hash = Fnv1a(Val, Vlen);
    PacketConstruct(&TxFrame, Entry->ProviderAddr, MakeService(ServiceType::Subscriptions, (uint8_t)SubCid::ValueUpdate),
                    Entry->Trid, FLAG_START | FLAG_STOP, (const uint8_t *)&Hash, sizeof(Hash),
                    PRIORITY_SUB_HIGH);
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
    // Docs: the value update is "sent as response packet, request is confirmation IF NEEDED" -
    // only OnChangeConfirm repeats until confirmed. Confirming every trigger would also
    // overwrite the provider's Hash/Hashlike state, which the delta trigger uses as its last
    // sent scalar value.
    const bool Confirm = Entry->Sub.Trigger == TriggerType::OnChangeConfirm;
    ApplyRequesterValue(Entry, Frame.Payload, PayloadBytes(Frame), Confirm);
}
#endif // USE_SUB_REQUEST
