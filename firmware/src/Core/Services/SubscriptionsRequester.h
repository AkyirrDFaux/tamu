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
inline bool SubscriptionsService::RequesterOccupied(const RequesterEntry &e) { return e.Active; }

inline RequesterEntry* SubscriptionsService::RequesterFindByTrid(uint16_t Trid) {
    return SubTableFindByTrid(RequesterTable, Trid, RequesterOccupied);
}

inline void SubscriptionsService::RequesterClearEntry(RequesterEntry* e) {
    *e = RequesterEntry{};
}

// Deletes `e` and compacts the table, so the active entries stay a TRID-sorted prefix
// (Docs: "stored in a sequential table, sorted by TRID").
inline void SubscriptionsService::RequesterRemove(RequesterEntry* e) {
    int Idx = (int)(e - RequesterTable);
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
    RequesterEntry* e = RequesterFindByTrid(Trid);
    if (e) return e;
    return RequesterAlloc(Trid);
}

// Applies a received value to the requester's target register (raw bytes) and confirms with
// the FNV-1a hash of the received bytes.
inline void SubscriptionsService::ApplyRequesterValue(RequesterEntry *e, const uint8_t *Val, uint8_t Vlen, bool Confirm) {
    // A value update renews the 120 s lease even when the target register is momentarily
    // unresolvable; otherwise a briefly unavailable target would let the entry expire and
    // cancel its still-live provider.
    e->lastValueMs = DeviceStatus.UptimeMs;
    e->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);

    FieldResult Fr = SubscriptionsGetField(e->targetReg);
    if (!Fr.Data) return;
    if (Vlen > Fr.Descriptor.Size) Vlen = Fr.Descriptor.Size;

    // The System block (type 0) is not a subscription target; every other block kind is
    // written through the shared Register setter (script inputs, dynamic entries, statics).
    if (!(BlockInfoType(e->targetReg) == 0 && BlockInfoInstance(e->targetReg) == 0)) {
        ValueInfo Meta = Fr.Descriptor;
        Meta.Size = Vlen;
        RegisterSetByBlockInfo(e->targetReg, Meta, Val, Vlen);
    }

    if (!Confirm) return;

    // Confirmation: the requester sends the hash of the received value (docs CID 0: the
    // confirmation is a request, no FLAG_TYPE), at the trigger's high priority.
    uint32_t h = Fnv1a(Val, Vlen);
    PacketFrame Reply;
    PacketConstruct(&Reply, e->providerAddr, MakeService(ServiceType::Subscriptions, 0),
                    e->Trid, FLAG_START | FLAG_STOP, (const uint8_t *)&h, sizeof(h),
                    PRIORITY_SUB_HIGH);
    SendAndVerifyPacket(Reply);
}

inline void SubscriptionsService::HandleRequesterValueUpdate(const PacketFrame &Frame) {
    if (!(Frame.Flags & FLAG_TYPE)) return;
    RequesterEntry* e = RequesterFindByTrid(Frame.Trid);
    if (!e) {
        // The sender holds a subscription this device does not have: an orphan provider entry.
        // That happens when a cancel could not land (the node was offline, or the verified
        // retries ran out) and is the *only* notification we get - the node's table is its own.
        // Cancel it here, for its TRID, back to the sender. Nothing happens in the normal case,
        // and if this cancel is lost too the orphan simply announces itself again on its next
        // period, so the table converges on its own.
        PacketFrame Cancel;
        PacketConstruct(&Cancel, Frame.IdSrc, MakeService(ServiceType::Subscriptions, 1),
                        Frame.Trid, FLAG_START | FLAG_STOP, nullptr, 0);
        SendAndVerifyPacket(Cancel);
        return;
    }
    // Docs: the value update is "sent as response packet, request is confirmation IF NEEDED" -
    // only OnChangeConfirm repeats until confirmed. Confirming every trigger would also
    // overwrite the provider's Hash/Hashlike state, which the delta trigger uses as its last
    // sent scalar value.
    const bool Confirm = e->Sub.Trigger == TriggerType::OnChangeConfirm;
    ApplyRequesterValue(e, Frame.Payload, PayloadBytes(Frame), Confirm);
}
#endif // USE_SUB_REQUEST
