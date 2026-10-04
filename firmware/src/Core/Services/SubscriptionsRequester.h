#pragma once

// Requester core (USE_SUB_REQUEST): the index table and triggers.
//
// Part of Core/Services/Subscriptions.h (included from there).

#include "Core/Services/SubscriptionsDefs.h"


// ===========================================================================
// Requester core (USE_SUB_REQUEST) - Tamu. TRID-sorted table; entries persisted to a file
// (1:1 copy minus the non-persistent timeout, regenerated at boot).
// ===========================================================================
#ifdef USE_SUB_REQUEST
#define MAX_REQUESTER_SUBS 16

// Docs "Requester table entry" (28 B wire): providerAddr, TRID, the shared subscription
// table, target register, timeout. The timeout is local uptime and is not persistent.
struct RequesterEntry {
    uint16_t providerAddr = 0;
    uint16_t trid = 0;
    SubscriptionTable sub;
    uint32_t targetReg = 0;      // local write target
    uint32_t timeout = 0;        // local uptime deadline (not persisted)
    bool active = false;
    // Initialization tracking (transient): the entry is "initialized" once a value update
    // arrives; until then the requester re-sends the registration (CID 1) to wake the provider.
    uint32_t registeredAtMs = 0;   // start of the initialization window (set/boot)
    uint32_t lastRegisteredMs = 0; // last re-registration sent
    uint32_t lastValueMs = 0;      // uptime of the last received value (0 = never)
};

#define SUB_RETRY_MS          100   // re-registration interval while un-initialized
#define SUB_INIT_WINDOW_MS    10000 // give up re-registering after this long
#define SUB_KEEPALIVE_MS      60000 // renew the provider's 120 s lease once values flow

static RequesterEntry requesterTable[MAX_REQUESTER_SUBS];

// The wire prefix of the entry IS the struct prefix, so serialization is a plain copy of the
// first 28 bytes (Docs "Requester table entry"): providerAddr, trid, table, targetReg, timeout.
static_assert(offsetof(RequesterEntry, providerAddr) == 0, "wire order: providerAddr first");
static_assert(offsetof(RequesterEntry, trid) == 2, "wire order: trid after providerAddr");
static_assert(offsetof(RequesterEntry, sub) == 4, "wire order: subscription table after trid");
static_assert(offsetof(RequesterEntry, targetReg) == 20, "wire order: targetReg after the table");
static_assert(offsetof(RequesterEntry, timeout) == 24, "wire order: timeout after targetReg");
#define REQUESTER_ENTRY_WIRE_SIZE 28
#define REQUESTER_FILE_ENTRY_SIZE 24 // the wire entry minus the timeout (not persisted)

// What makes a requester entry occupied (used by the shared table search).
static bool RequesterOccupied(const RequesterEntry &e) { return e.active; }

static RequesterEntry* RequesterFindByTrid(uint16_t trid) {
    return SubTableFindByTrid(requesterTable, trid, RequesterOccupied);
}

static void RequesterClearEntry(RequesterEntry* e) {
    *e = RequesterEntry{};
}

// Deletes `e` and compacts the table, so the active entries stay a TRID-sorted prefix
// (Docs: "stored in a sequential table, sorted by TRID").
static void RequesterRemove(RequesterEntry* e) {
    int idx = (int)(e - requesterTable);
    for (int i = idx; i < MAX_REQUESTER_SUBS - 1; i++) requesterTable[i] = requesterTable[i + 1];
    RequesterClearEntry(&requesterTable[MAX_REQUESTER_SUBS - 1]);
}

// Finds the entry for `trid`, or allocates it at its sorted position. Returns nullptr when the
// table is full. The new entry is marked active with only its TRID set.
static RequesterEntry* RequesterUpsert(uint16_t trid) {
    int count = 0;
    while (count < MAX_REQUESTER_SUBS && requesterTable[count].active) {
        if (requesterTable[count].trid == trid) return &requesterTable[count];
        count++;
    }
    if (count >= MAX_REQUESTER_SUBS) return nullptr;
    int pos = 0;
    while (pos < count && requesterTable[pos].trid < trid) pos++;
    for (int i = count; i > pos; i--) requesterTable[i] = requesterTable[i - 1];
    RequesterClearEntry(&requesterTable[pos]);
    requesterTable[pos].trid = trid;
    requesterTable[pos].active = true;
    return &requesterTable[pos];
}

// Applies a received value to the requester's target register (raw bytes) and confirms with
// the FNV-1a hash of the received bytes.
static void ApplyRequesterValue(RequesterEntry *e, const uint8_t *val, uint8_t vlen, bool confirm = true) {
    FieldResult fr = SubscriptionsGetField(e->targetReg);
    if (!fr.Data) return;
    if (vlen > fr.Descriptor.Size) vlen = fr.Descriptor.Size;

    // The System block (type 0) is not a subscription target; every other block kind is
    // written through the shared Register setter (script inputs, dynamic entries, statics).
    if (!(BlockInfoType(e->targetReg) == 0 && BlockInfoInstance(e->targetReg) == 0)) {
        ValueInfo meta = fr.Descriptor;
        meta.Size = vlen;
        RegisterSetByBlockInfo(e->targetReg, meta, val, vlen);
    }

    e->lastValueMs = DeviceStatus.UptimeMs;
    e->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs); // a value renews the 120 s lease
    if (!confirm) return;

    // Confirmation: the requester sends the hash of the received value (docs CID 0: the
    // confirmation is a request, no FLAG_TYPE).
    uint32_t h = Fnv1a(val, vlen);
    PacketFrame reply;
    PacketConstruct(&reply, e->providerAddr, MakeService(ServiceType::Subscriptions, 0),
                    e->trid, FLAG_START | FLAG_STOP, (const uint8_t *)&h, sizeof(h));
    SendAndVerifyPacket(reply);
}

static void HandleRequesterValueUpdate(const PacketFrame &frame) {
    if (!(frame.flags & FLAG_TYPE)) return;
    RequesterEntry* e = RequesterFindByTrid(frame.trid);
    if (!e) {
        // The sender holds a subscription this device does not have: an orphan provider entry.
        // That happens when a cancel could not land (the node was offline, or the verified
        // retries ran out) and is the *only* notification we get - the node's table is its own.
        // Cancel it here, for its TRID, back to the sender. Nothing happens in the normal case,
        // and if this cancel is lost too the orphan simply announces itself again on its next
        // period, so the table converges on its own.
        PacketFrame cancel;
        PacketConstruct(&cancel, frame.id_src, MakeService(ServiceType::Subscriptions, 1),
                        frame.trid, FLAG_START | FLAG_STOP, nullptr, 0);
        SendAndVerifyPacket(cancel);
        return;
    }
    // Docs: the value update is "sent as response packet, request is confirmation IF NEEDED" -
    // only OnChangeConfirm repeats until confirmed. Confirming every trigger would also
    // overwrite the provider's Hash/Hashlike state, which the delta trigger uses as its last
    // sent scalar value.
    const bool confirm = e->sub.trigger == TriggerType::OnChangeConfirm;
    ApplyRequesterValue(e, frame.payload, PayloadBytes(frame), confirm);
}
#endif // USE_SUB_REQUEST
