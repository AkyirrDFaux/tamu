#pragma once
#include "Core/Functions/Packet.h"
#include "Core/Functions/TimeSync.h"
#include "Core/Functions/MemoryTypes.h"
#include "Core/Functions/Dispatcher.h"
#include "Core/Services/Register.h"
#include "Core/Services/Storage.h"
#include "Core/Types/Enums.h"

// Reduced subscription service (Docs/Services/Subscriptions.md).
//   USE_SUB_PROVIDE - provider side: sends source values (raw bytes) with hash-based
//                     on-change detection, checked from the main loop.
//   USE_SUB_REQUEST - requester side (Tamu): stores target/source, applies values with the
//                     foreign-origin flag and confirms with a hash.
// Value updates carry the RAW value bytes (no TLFV header); the requester's confirmation
// is the 4-byte FNV-1a hash of the received bytes.

#define TRID_SUB_BASE 0xFA00
#define TRID_SUB_MAX  0xFBFF

// FNV-1a hash over the value bytes (provider on-change detection + requester confirmation).
static inline uint32_t Fnv1a(const uint8_t *data, uint8_t len) {
    uint32_t hash = 0x811C9DC5u;
    for (uint8_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    return hash;
}

// Reads the current value of the register addressed by a 32-bit BlockInfo.
static inline FieldResult SubscriptionsGetField(uint32_t blockInfo) {
    uint16_t type = BlockInfoType(blockInfo);
    uint8_t inst = BlockInfoInstance(blockInfo);
    uint8_t field = BlockInfoField(blockInfo);
    uint8_t key = BlockInfoKey(blockInfo);

    if (type == 0 && inst == 0) {
        BlockMeta m;
        uint8_t vbuf[24];
        uint8_t vsz = 0;
        if (RegisterGetSystemField(field, key, m, vbuf, vsz)) {
            FieldResult fr;
            fr.Descriptor = m;
            fr.Data = vbuf;
            return fr;
        }
        return FieldResult{};
    }
#ifndef DISABLE_DYNAMIC_MEMORY
    if (type == 0x3FF) {
        if (inst < dynamic_block_registry.block_count) {
            DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
            if (block) return block->Get(field);
        }
        return FieldResult{};
    }
#endif
    int idx = FindStaticBlock(type, inst);
    if (idx < 0) return FieldResult{};
    return static_block_registry[idx].Get(field);
}

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
    bool active = false;
};

static RequesterEntry requesterTable[MAX_REQUESTER_SUBS];

static RequesterEntry* RequesterFindByTrid(uint16_t trid) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (requesterTable[i].active && requesterTable[i].trid == trid)
            return &requesterTable[i];
    }
    return nullptr;
}

static RequesterEntry* RequesterFindFree() {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (!requesterTable[i].active)
            return &requesterTable[i];
    }
    return nullptr;
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
    if (type == 0 && inst == 0) return;

    int idx = FindStaticBlock(type, inst);
    if (idx >= 0) {
        BlockMeta meta = static_block_registry[idx].Schema->Map[field];
        meta.FlagsAndType = newFlags;
        static_block_registry[idx].Set(field, val, vlen, meta.FlagsAndType);
    }
#ifndef DISABLE_DYNAMIC_MEMORY
    else if (type == 0x3FF) {
        if (inst < dynamic_block_registry.block_count) {
            DynamicBlockDescriptor* block = dynamic_block_registry.GetBlock(inst);
            if (block) {
                BlockMeta meta;
                meta.FlagsAndType = newFlags;
                meta.Size = vlen;
                block->Set(field, val, vlen, meta.FlagsAndType);
            }
        }
    }
#endif
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
    if (!e) return;
    ApplyRequesterValue(e, frame.payload, PayloadBytes(frame), true);
}

// Serializes one requester entry (wire order: providerAddr, trid, targetReg, sourceReg,
// trigger + 3 pad, period, min = 24 B). Returns the advanced offset.
static uint16_t RequesterEntrySerialize(uint8_t *buf, uint16_t off, const RequesterEntry *e) {
    *(uint16_t *)(buf + off) = e->providerAddr; off += 2;
    *(uint16_t *)(buf + off) = e->trid; off += 2;
    *(uint32_t *)(buf + off) = e->targetReg; off += 4;
    *(uint32_t *)(buf + off) = e->sourceReg; off += 4;
    buf[off++] = (uint8_t)e->trigger;
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; // 24-bit padding
    *(uint32_t *)(buf + off) = e->periodMs; off += 4;
    *(uint32_t *)(buf + off) = e->minTimeMs; off += 4;
    return off;
}
#endif // USE_SUB_REQUEST

// ===========================================================================
// Provider (USE_SUB_PROVIDE): active until canceled, not persistent. Updates checked
// from the main loop (SubscriptionsTick -> EvaluateProviderTriggers).
// ===========================================================================
#ifdef USE_SUB_PROVIDE
#ifdef BOARD_DAS_v0_1
#define MAX_PROVIDER_SUBS 4 // the DAS's 2 KB RAM: a couple of active providers suffice
#else
#define MAX_PROVIDER_SUBS 20
#endif

// Provider entry (24 B): requesterAddr, trid, sourceReg, trigger(+24 pad), period,
// min/retry interval, last sent, FNV-1a hash of the last confirmed value.
struct ProviderEntry {
    uint16_t requesterAddr = 0;  // 0 = invalid entry
    uint16_t trid = 0;
    uint32_t sourceReg = 0;
    TriggerType trigger = TriggerType::Periodic;
    uint32_t periodMs = 0;
    uint32_t minTimeMs = 0;      // minimum interval / retry interval
    uint32_t lastSentMs = 0;
    uint32_t hash = 0;
};

static ProviderEntry providerTable[MAX_PROVIDER_SUBS];

static uint16_t SubscriptionsNextTrid() {
    static uint16_t next = TRID_SUB_BASE;
    for (int tries = 0; tries < (TRID_SUB_MAX - TRID_SUB_BASE + 1); tries++) {
        uint16_t cand = next++;
        if (cand > TRID_SUB_MAX) { next = TRID_SUB_BASE; cand = next++; }
        bool used = false;
#ifdef USE_SUB_REQUEST
        for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
            if (requesterTable[i].active && requesterTable[i].trid == cand) { used = true; break; }
        }
#endif
        for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
            if (providerTable[i].requesterAddr != 0 && providerTable[i].trid == cand) { used = true; break; }
        }
        if (!used) return cand;
    }
    return 0;
}

static ProviderEntry* ProviderFindByTrid(uint16_t trid) {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        if (providerTable[i].requesterAddr != 0 && providerTable[i].trid == trid)
            return &providerTable[i];
    }
    return nullptr;
}

static ProviderEntry* ProviderFindFree() {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        if (providerTable[i].requesterAddr == 0)
            return &providerTable[i];
    }
    return nullptr;
}

static void ProviderClearEntry(ProviderEntry* e) {
    *e = ProviderEntry{};
}

// Serializes one provider entry in the CID 2 wire format (requesterAddr, trid, sourceReg,
// trigger + 3 pad, period, min, lastSent, hash = 24 B).
static uint16_t ProviderEntrySerialize(uint8_t *buf, uint16_t off, const ProviderEntry *e) {
    *(uint16_t *)(buf + off) = e->requesterAddr; off += 2;
    *(uint16_t *)(buf + off) = e->trid; off += 2;
    *(uint32_t *)(buf + off) = e->sourceReg; off += 4;
    buf[off++] = (uint8_t)e->trigger;
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; // 24-bit padding
    *(uint32_t *)(buf + off) = e->periodMs; off += 4;
    *(uint32_t *)(buf + off) = e->minTimeMs; off += 4;
    *(uint32_t *)(buf + off) = e->lastSentMs; off += 4;
    *(uint32_t *)(buf + off) = e->hash; off += 4;
    return off;
}

// The provider side of the requester's confirmation (CID 0 request): the payload is the
// FNV-1a hash of the value the requester received; confirm-required triggers stop here.
static void HandleProviderConfirmation(const PacketFrame &frame) {
    ProviderEntry* e = ProviderFindByTrid(frame.trid);
    if (!e) return;
    if (PayloadBytes(frame) >= 4) {
        uint32_t h;
        memcpy(&h, frame.payload, 4);
        e->hash = h;
    }
}

static void EvaluateProviderTriggers(uint32_t nowMs) {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        ProviderEntry* e = &providerTable[i];
        if (e->requesterAddr == 0) continue;
        FieldResult fr = SubscriptionsGetField(e->sourceReg);
        if (!fr.Data) continue;
        uint8_t vlen = fr.Descriptor.Size;
        if (vlen > MAX_PAYLOAD_SIZE) vlen = MAX_PAYLOAD_SIZE;
        uint32_t hash = Fnv1a((const uint8_t *)fr.Data, vlen);
        uint32_t elapsed = nowMs - e->lastSentMs;

        bool send = false;
        switch (e->trigger) {
        case TriggerType::Periodic:
            send = (e->periodMs > 0) && (elapsed >= e->periodMs);
            if (send) e->hash = hash;
            break;
        case TriggerType::OnChangePeriodic:
            if (hash != e->hash && elapsed >= e->minTimeMs) { send = true; e->hash = hash; }
            else if (e->periodMs > 0 && elapsed >= e->periodMs) { send = true; e->hash = hash; }
            break;
        case TriggerType::OnChangeConfirm:
            send = (hash != e->hash) && (elapsed >= e->minTimeMs);
            break; // hash updates only on confirmation
        default:
            break;
        }
        if (!send) continue;

        e->lastSentMs = nowMs;

        // Self-loopback (provider and requester on the same device): apply locally; a
        // confirm-required trigger is confirmed immediately (no bus round-trip).
        bool applied = false;
#ifdef USE_SUB_REQUEST
        if (e->requesterAddr == DeviceStatus.ShortAddress) {
            RequesterEntry* r = RequesterFindByTrid(e->trid);
            if (r) { ApplyRequesterValue(r, (const uint8_t *)fr.Data, vlen, false); applied = true; }
            if (e->trigger == TriggerType::OnChangeConfirm) e->hash = hash;
        }
#endif
        if (!applied) {
            PacketConstruct(&tx_frame, e->requesterAddr,
                            MakeService(ServiceType::Subscriptions, 0),
                            e->trid,
                            FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_REQACK,
                            (const uint8_t *)fr.Data, vlen);
            SendAndVerifyPacket(tx_frame);
        }
    }
}
#endif // USE_SUB_PROVIDE

// ===========================================================================
// Requester persistence + provider re-registration (USE_SUB_REQUEST).
// ===========================================================================
#ifdef USE_SUB_REQUEST
static const char* SubscriptionsRequesterFile = "SUBREQ";

static void SaveRequesterTable() {
    uint8_t buf[1 + MAX_REQUESTER_SUBS * 22]; // entries without the TRID (non-persistent)
    uint16_t off = 0;
    uint8_t count = 0;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) if (requesterTable[i].active) count++;
    buf[off++] = count;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* e = &requesterTable[i];
        if (!e->active) continue;
        *(uint32_t *)(buf + off) = e->targetReg; off += 4;
        *(uint32_t *)(buf + off) = e->sourceReg; off += 4;
        *(uint16_t *)(buf + off) = e->providerAddr; off += 2;
        buf[off++] = (uint8_t)e->trigger;
        buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
        *(uint32_t *)(buf + off) = e->periodMs; off += 4;
        *(uint32_t *)(buf + off) = e->minTimeMs; off += 4;
    }
    static const char tmp_name[8] = {'S','U','B','R','E','Q','~',' '};
    if (Storage.FileExists(tmp_name) != 0xFFFFFFFF)
        Storage.DeleteFile(tmp_name);
    if (!Storage.CreateFile(tmp_name, off)) return;
    if (!Storage.WriteToFile(tmp_name, 0, off, (const char*)buf)) { Storage.DeleteFile(tmp_name); return; }
    if (!Storage.RenameFile(tmp_name, SubscriptionsRequesterFile)) { Storage.DeleteFile(tmp_name); return; }
}

static void LoadRequesterTable() {
    uint8_t buf[256];
    uint16_t len = Storage.ReadFromFile(SubscriptionsRequesterFile, 0, sizeof(buf), (char*)buf);
    if (len == 0) return;

    uint16_t off = 0;
    uint8_t count = buf[off++];
    for (uint8_t i = 0; i < count && off + 22 <= len; i++) {
        RequesterEntry* e = RequesterFindFree();
        if (!e) break;
        e->targetReg = *(uint32_t *)(buf + off); off += 4;
        e->sourceReg = *(uint32_t *)(buf + off); off += 4;
        e->providerAddr = *(uint16_t *)(buf + off); off += 2;
        e->trigger = (TriggerType)buf[off++];
        off += 3; // padding
        e->periodMs = *(uint32_t *)(buf + off); off += 4;
        e->minTimeMs = *(uint32_t *)(buf + off); off += 4;
        e->trid = SubscriptionsNextTrid();
        if (e->trid == 0) e->trid = TRID_SUB_BASE;
        e->active = true;
    }
}

// Re-registers the (non-persistent) provider side for a requester subscription so a
// restored table keeps pushing values after boot. Same-device providers get a direct
// provider-table entry; remote providers get a CID 1 "Change subscription" packet.
static void RegisterRequesterProvider(RequesterEntry* e) {
    if (!e || !e->active) return;

#ifdef USE_SUB_PROVIDE
    if (e->providerAddr == DeviceStatus.ShortAddress) {
        ProviderEntry* p = ProviderFindByTrid(e->trid);
        if (!p) { p = ProviderFindFree(); if (!p) return; }
        p->requesterAddr = DeviceStatus.ShortAddress;
        p->trid = e->trid;
        p->sourceReg = e->sourceReg;
        p->trigger = e->trigger;
        p->periodMs = e->periodMs;
        p->minTimeMs = e->minTimeMs;
        p->lastSentMs = 0;
        p->hash = 0;
        return;
    }
#endif
    // Remote provider: send the subscription info (CID 1, fire and forget).
    uint8_t payload[24]; uint16_t off = 0;
    *(uint32_t *)(payload + off) = e->targetReg; off += 4;
    *(uint32_t *)(payload + off) = e->sourceReg; off += 4;
    *(uint16_t *)(payload + off) = DeviceStatus.ShortAddress; off += 2;
    payload[off++] = (uint8_t)e->trigger;
    payload[off++] = 0; payload[off++] = 0; payload[off++] = 0;
    *(uint32_t *)(payload + off) = e->periodMs; off += 4;
    *(uint32_t *)(payload + off) = e->minTimeMs; off += 4;
    PacketFrame req;
    PacketConstruct(&req, e->providerAddr, MakeService(ServiceType::Subscriptions, 1),
                    e->trid, FLAG_START | FLAG_STOP, payload, off);
    SendAndVerifyPacket(req);
}

void ReRegisterSubscriptions() {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (requesterTable[i].active)
            RegisterRequesterProvider(&requesterTable[i]);
    }
}
#endif // USE_SUB_REQUEST

// Shared reply for every subscription CID (docs: responses are packets with FLAG_TYPE).
static void SubReply(const PacketFrame &frame, const uint8_t *payload, uint16_t len) {
    PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                    FLAG_TYPE | FLAG_START | FLAG_STOP, payload, len);
#ifdef USE_APP_INTERFACE
    if (frame.id_src == 0xFFFE) {
        AppInterfaceSend(tx_frame);
    } else {
        DispatchPacket(tx_frame);
    }
#else
    SendAndVerifyPacket(tx_frame);
#endif
}

__attribute__((noinline)) static void HandleSubscriptions(const PacketFrame &frame) {
    uint8_t cid = GetServiceCID(frame.srv_tgt);

    switch (cid) {
        case 0: {
#ifdef USE_SUB_REQUEST
            // Provider value update (response): apply + confirm (requester role).
            HandleRequesterValueUpdate(frame);
#endif
#ifdef USE_SUB_PROVIDE
            // Requester confirmation (request): update the provider's hash.
            if (!(frame.flags & FLAG_TYPE))
                HandleProviderConfirmation(frame);
#endif
            break;
        }

        case 1: { // Change subscription (provider side).
#ifdef USE_SUB_PROVIDE
            if (frame.payload_len == 0) {
                ProviderEntry* e = ProviderFindByTrid(frame.trid);
                if (e) ProviderClearEntry(e);
                uint8_t resp = 1;
                SubReply(frame, &resp, 1);
                break;
            }
            if (PayloadBytes(frame) < 4 + 4 + 2 + 1 + 4 + 4) break;

            uint16_t offset = 0;
            offset += 4; // targetReg (not used by provider)
            uint32_t sourceReg = *(uint32_t *)(frame.payload + offset); offset += 4;
            uint16_t requesterAddr = *(uint16_t *)(frame.payload + offset); offset += 2;
            TriggerType trigger = (TriggerType)frame.payload[offset++];
            offset += 3; // padding
            uint32_t periodMs = *(uint32_t *)(frame.payload + offset); offset += 4;
            uint32_t minTimeMs = *(uint32_t *)(frame.payload + offset); offset += 4;

            ProviderEntry* e = ProviderFindByTrid(frame.trid);
            bool isNew = false;
            if (!e) {
                e = ProviderFindFree();
                if (!e) break;
                isNew = true;
            }
            if (isNew) {
                e->trid = frame.trid ? frame.trid : SubscriptionsNextTrid();
                if (e->trid == 0) break;
            }
            e->sourceReg = sourceReg;
            e->requesterAddr = requesterAddr;
            e->trigger = trigger;
            e->periodMs = periodMs;
            e->minTimeMs = minTimeMs;
            e->lastSentMs = 0;
            e->hash = 0;
            uint8_t resp = 1;
            SubReply(frame, &resp, 1);
#endif
            break;
        }

        case 2: { // Get subscriptions (provider).
#ifdef USE_SUB_PROVIDE
            if (frame.payload_len == 0) {
                uint8_t count = 0;
                for (int i = 0; i < MAX_PROVIDER_SUBS; i++) if (providerTable[i].requesterAddr != 0) count++;
                uint8_t buf[1 + MAX_PROVIDER_SUBS * 24];
                uint16_t off = 0;
                buf[off++] = count;
                for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
                    if (providerTable[i].requesterAddr == 0) continue;
                    off = ProviderEntrySerialize(buf, off, &providerTable[i]);
                }
                SubReply(frame, buf, off);
            } else if (PayloadBytes(frame) >= 1) {
                uint8_t index = frame.payload[0];
                if (index < MAX_PROVIDER_SUBS && providerTable[index].requesterAddr != 0) {
                    uint8_t buf[24];
                    uint16_t off = ProviderEntrySerialize(buf, 0, &providerTable[index]);
                    SubReply(frame, buf, off);
                }
            }
#else
            uint8_t resp = 0;
            SubReply(frame, &resp, 1);
#endif
            break;
        }

        case 3: { // Get subscriptions (requester).
#ifdef USE_SUB_REQUEST
            if (frame.payload_len == 0) {
                uint8_t count = 0;
                for (int i = 0; i < MAX_REQUESTER_SUBS; i++) if (requesterTable[i].active) count++;
                uint8_t buf[1 + MAX_REQUESTER_SUBS * 24];
                uint16_t off = 0;
                buf[off++] = count;
                for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
                    if (!requesterTable[i].active) continue;
                    off = RequesterEntrySerialize(buf, off, &requesterTable[i]);
                }
                SubReply(frame, buf, off);
            } else if (PayloadBytes(frame) >= 1) {
                uint8_t index = frame.payload[0];
                if (index < MAX_REQUESTER_SUBS && requesterTable[index].active) {
                    uint8_t buf[24];
                    uint16_t off = RequesterEntrySerialize(buf, 0, &requesterTable[index]);
                    SubReply(frame, buf, off);
                }
            }
#else
            uint8_t resp = 0;
            SubReply(frame, &resp, 1);
#endif
            break;
        }

        case 4: { // Set subscription (requester).
#ifdef USE_SUB_REQUEST
            if (PayloadBytes(frame) >= 1) {
                uint8_t index = frame.payload[0];
                if (index < MAX_REQUESTER_SUBS) {
                    if (frame.payload_len == 1) {
                        // Delete + compact so the app's ordinal index stays in sync.
                        for (int i = index; i < MAX_REQUESTER_SUBS - 1; i++)
                            requesterTable[i] = requesterTable[i + 1];
                        RequesterClearEntry(&requesterTable[MAX_REQUESTER_SUBS - 1]);
                        SaveRequesterTable();
                        uint8_t resp = 1;
                        SubReply(frame, &resp, 1);
                    } else {
                        if (PayloadBytes(frame) < 1 + 2 + 2 + 4 + 4 + 1 + 4 + 4) break;
                        uint16_t offset = 1;
                        RequesterEntry* e = &requesterTable[index];
                        e->providerAddr = *(uint16_t *)(frame.payload + offset); offset += 2;
                        offset += 2; // payload TRID field: the frame TRID is authoritative
                        e->targetReg = *(uint32_t *)(frame.payload + offset); offset += 4;
                        e->sourceReg = *(uint32_t *)(frame.payload + offset); offset += 4;
                        e->trigger = (TriggerType)frame.payload[offset++];
                        offset += 3; // padding
                        e->periodMs = *(uint32_t *)(frame.payload + offset); offset += 4;
                        e->minTimeMs = *(uint32_t *)(frame.payload + offset); offset += 4;
                        e->trid = frame.trid ? frame.trid : SubscriptionsNextTrid();
                        e->active = true;
                        SaveRequesterTable();

                        // Respond first (the app waits on the transaction ID), then register
                        // the provider side (same-device or CID 1 to the remote provider).
                        uint8_t resp = 1;
                        PacketFrame reply;
                        PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.trid,
                                        FLAG_TYPE | FLAG_START | FLAG_STOP, &resp, 1);
#ifdef USE_APP_INTERFACE
                        if (frame.id_src == 0xFFFE) AppInterfaceSend(reply); else DispatchPacket(reply);
#else
                        DispatchPacket(reply);
#endif
                        RegisterRequesterProvider(e);
                    }
                }
            }
#else
            if (PayloadBytes(frame) >= 1) {
                uint8_t index = frame.payload[0];
                if (index < MAX_PROVIDER_SUBS && frame.payload_len == 1) {
                    ProviderClearEntry(&providerTable[index]);
                    uint8_t resp = 1;
                    SubReply(frame, &resp, 1);
                }
            }
#endif
            break;
        }

        case 5: { // Save the requester table to its file.
#ifdef USE_SUB_REQUEST
            SaveRequesterTable();
#endif
            uint8_t resp = 1;
            SubReply(frame, &resp, 1);
            break;
        }
    }
}

void SubscriptionsTick(uint32_t nowMs) {
#ifdef USE_SUB_PROVIDE
    EvaluateProviderTriggers(nowMs);
#endif
}