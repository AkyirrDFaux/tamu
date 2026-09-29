#pragma once

// Requester core (USE_SUB_REQUEST): the index table and triggers.
//
// Part of Core/Services/Subscriptions.h (included from there).

#include "Core/Services/SubscriptionsDefs.h"


// ===========================================================================
// Requester core (USE_SUB_REQUEST) - Tamu. Index-based table; entries persisted to a file
// (1:1 copy minus the non-persistent TRID, regenerated at boot).
// ===========================================================================
#ifdef USE_SUB_REQUEST
#define MAX_REQUESTER_SUBS 16

struct RequesterEntry {
    uint16_t providerAddr = 0;
    uint16_t trid = 0;
    uint32_t targetReg = 0;
    uint32_t sourceReg = 0;
    TriggerType trigger = TriggerType::Periodic;
    uint32_t periodMs = 0;
    uint32_t minTimeMs = 0; // minimum interval / retry interval
    Number deadzone = N(0); // for number/vector triggers (forwarded to the provider)
    // Initialization tracking: the entry is "initialized" once a value update arrives.
    // Until then the requester re-sends the registration (CID 1) to wake the provider.
    uint32_t registeredAtMs = 0;  // start of the initialization window (set/boot)
    uint32_t lastRegisteredMs = 0; // last re-registration sent
    uint32_t lastValueMs = 0;     // uptime of the last received value (0 = never)
    bool active = false;
};

#define SUB_RETRY_MS          100   // re-registration interval while un-initialized
#define SUB_INIT_WINDOW_MS    10000 // give up re-registering after this long

static RequesterEntry requesterTable[MAX_REQUESTER_SUBS];

// What makes a requester entry occupied (used by the shared table search).
static bool RequesterOccupied(const RequesterEntry &e) { return e.active; }

static RequesterEntry* RequesterFindByTrid(uint16_t trid) {
    return SubTableFindByTrid(requesterTable, trid, RequesterOccupied);
}

static RequesterEntry* RequesterFindFree() {
    return SubTableFindFree(requesterTable, RequesterOccupied);
}

static void RequesterClearEntry(RequesterEntry* e) {
    *e = RequesterEntry{};
}

// Applies a received value to the requester's target register (raw bytes), setting the
// foreign-origin flag, and confirms with the FNV-1a hash of the received bytes.
static void ApplyRequesterValue(RequesterEntry *e, const uint8_t *val, uint8_t vlen, bool confirm = true) {
    FieldResult fr = SubscriptionsGetField(e->targetReg);
    if (!fr.Data) return;
    if (vlen > fr.Descriptor.Size) vlen = fr.Descriptor.Size;

    uint16_t newFlags = fr.Descriptor.FlagsAndType | FieldFlags::External;
    uint16_t type = BlockInfoType(e->targetReg);
    uint8_t inst = BlockInfoInstance(e->targetReg);
    uint8_t field = BlockInfoField(e->targetReg);
    uint8_t key = BlockInfoKey(e->targetReg);
    if (type == 0 && inst == 0) return;

#ifdef USE_SCRIPTS
    if (type == 0x3FE) {
        // Script I/O target: only inputs are writable (outputs are read-only).
        BlockMeta meta;
        meta.FlagsAndType = newFlags;
        meta.Key = key;
        meta.Size = vlen;
        ScriptSetEntry(inst, field, key, meta, val, vlen);
    } else
#endif
    {
        int idx = FindStaticBlock(type, inst);
        if (idx >= 0) {
            BlockMeta meta = static_block_registry[idx].Schema->Map[field];
            meta.FlagsAndType = newFlags;
            if (static_block_registry[idx].Set(field, val, vlen, meta.FlagsAndType))
                StaticMarkWriteFromFlags((uint8_t)idx, field, newFlags); // External origin
        }
#ifndef DISABLE_DYNAMIC_MEMORY
        else if (type == 0x3FF) {
            if (inst < dynamic_block_registry.block_count) {
                DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
                if (block) {
                    BlockMeta meta;
                    meta.FlagsAndType = newFlags;
                    meta.Size = vlen;
                    block->SetEntry(field, key, val, vlen, meta.FlagsAndType);
                }
            }
        }
#endif
    }
    e->lastValueMs = DeviceStatus.UptimeMs;
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
    const bool confirm = e->trigger == TriggerType::OnChangeConfirm;
    ApplyRequesterValue(e, frame.payload, PayloadBytes(frame), confirm);
}

// Serializes one requester entry (wire order: providerAddr, trid, targetReg, sourceReg,
// trigger + 3 pad, period, min, deadzone = 28 B). Returns the advanced offset.
static uint16_t RequesterEntrySerialize(uint8_t *buf, uint16_t off, const RequesterEntry *e) {
    *(uint16_t *)(buf + off) = e->providerAddr; off += 2;
    *(uint16_t *)(buf + off) = e->trid; off += 2;
    *(uint32_t *)(buf + off) = e->targetReg; off += 4;
    *(uint32_t *)(buf + off) = e->sourceReg; off += 4;
    buf[off++] = (uint8_t)e->trigger;
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; // 24-bit padding
    *(uint32_t *)(buf + off) = e->periodMs; off += 4;
    *(uint32_t *)(buf + off) = e->minTimeMs; off += 4;
    *(uint32_t *)(buf + off) = (uint32_t)e->deadzone.Value; off += 4;
    return off;
}
#endif // USE_SUB_REQUEST
