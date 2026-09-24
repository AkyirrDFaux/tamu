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

// Packet priorities (Docs/RSBus and Packets.md: high-priority subscriptions sit above
// "Other" (default 8), low-priority ones below it).
#define SUB_PRIORITY_HIGH 4
#define SUB_PRIORITY_LOW  12

// True for the high-priority triggers (confirmation and edge detection).
static inline bool SubscriptionsHighPriority(TriggerType t) {
    return t == TriggerType::OnChangeConfirm || t == TriggerType::EdgeRising ||
           t == TriggerType::EdgeFalling || t == TriggerType::EdgeAny;
}

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
        // System fields are synthesised on the fly; keep the value in a shared buffer
        // (single-threaded main loop, and the caller consumes it before any nested call).
        static uint8_t s_sysFieldBuf[24];
        BlockMeta m;
        uint8_t vsz = 0;
        if (RegisterGetSystemField(field, key, m, s_sysFieldBuf, vsz)) {
            FieldResult fr;
            fr.Descriptor = m;
            fr.Data = s_sysFieldBuf;
            return fr;
        }
        return FieldResult{};
    }
#ifdef USE_SCRIPTS
    if (type == 0x3FE) { // Script I/O (inputs/outputs)
        BlockMeta m;
        void *p = nullptr;
        if (ScriptGetIoPointer(inst, field, key, m, p)) {
            FieldResult fr;
            fr.Descriptor = m;
            fr.Data = p;
            return fr;
        }
        return FieldResult{};
    }
#endif
#ifndef DISABLE_DYNAMIC_MEMORY
    if (type == 0x3FF) {
        if (inst < dynamic_block_registry.block_count) {
            DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
            if (block) {
                KeyResult kr = block->GetKey(field, key);
                FieldResult fr;
                fr.Descriptor = kr.meta;
                fr.Data = kr.data_ptr;
                return fr;
            }
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
            static_block_registry[idx].Set(field, val, vlen, meta.FlagsAndType);
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
    if (!e) return;
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

// Provider entry (32 B wire): requesterAddr, trid, sourceReg, trigger(+24 pad), period,
// min/retry interval, last sent, hash/hashlike, deadzone.
struct ProviderEntry {
    uint16_t requesterAddr = 0;  // 0 = invalid entry
    uint16_t trid = 0;
    uint32_t sourceReg = 0;
    TriggerType trigger = TriggerType::Periodic;
    uint32_t periodMs = 0;
    uint32_t minTimeMs = 0;      // minimum interval / retry interval
    uint32_t lastSentMs = 0;
    uint32_t hash = 0;           // FNV-1a hash / edge counter / last value / subresolution
    Number deadzone = N(0);      // for delta/edge triggers
    // Transient (not serialized): last boolean sample for edge detection and the last
    // counter value that was transmitted.
    bool lastBool = false;
    uint32_t sentCounter = 0;
    // Last SENT vector for a delta subscription on a Vector source (up to 3 axes); the
    // scalar delta reuses `hash` for the same purpose.
    int32_t lastVec[3] = {0, 0, 0};
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
// trigger + 3 pad, period, min, lastSent, hash, deadzone = 32 B).
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
    *(uint32_t *)(buf + off) = (uint32_t)e->deadzone.Value; off += 4;
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

// Per-value hashlike for the delta trigger: scalar -> the raw Number bits; vector ->
// the subresolution pack (Docs/Services/Subscriptions.md "Subresolution Vector"): 10 bits
// per axis (first 3 axes), each [5-bit above deadzone | 5-bit below], where "above" is a
// sign bit plus a 4-bit hash of the magnitude and "below" is the distance bucket 0..31.
static uint32_t SubscriptionsDeltaHash(const FieldResult &fr, Number deadzone) {
    uint16_t dtype = BlockMetaType(fr.Descriptor.FlagsAndType);
    uint8_t size = fr.Descriptor.Size;
    const uint8_t *data = (const uint8_t *)fr.Data;
#ifndef SCALAR_ONLY
    if (dtype == (uint16_t)DataType::Vector && size >= 4 && (size % 4) == 0) {
        uint8_t axes = (uint8_t)(size / 4);
        if (axes > 3) axes = 3;
        int32_t dz = deadzone.Value;
        uint32_t packed = 0;
        for (uint8_t a = 0; a < axes; a++) {
            int32_t raw = 0;
            memcpy(&raw, data + (size_t)a * 4, 4);
            int32_t av = raw < 0 ? -raw : raw;
            uint32_t above5 = ((raw < 0 ? 1u : 0u) << 4) | ((((uint32_t)av * 2654435761u) >> 28) & 0xF);
            uint32_t below5 = 0;
            if (dz > 0) {
                // av < dz here, so av*31 cannot overflow when dz is a 16.16 value.
                below5 = ((uint32_t)av * 31u) / (uint32_t)dz;
                if (below5 > 31) below5 = 31;
            }
            packed |= ((above5 << 5) | below5) << (10 * a);
        }
        return packed;
    }
#endif
    if (dtype == (uint16_t)DataType::Number && size >= 4) {
        int32_t raw = 0;
        memcpy(&raw, data, 4);
        return (uint32_t)raw;
    }
    return Fnv1a(data, size);
}

// True for a 4-byte scalar numeric value. Delta subscriptions gate these on the change
// magnitude against the deadzone (Docs "Checks distance ... Last scalar value"); the
// vector path uses [SubscriptionsDeltaHash]'s subresolution pack instead.
static bool SubscriptionsIsScalar(const FieldResult &fr) {
    if (fr.Descriptor.Size < 4) return false;
    uint16_t t = BlockMetaType(fr.Descriptor.FlagsAndType);
    return t == (uint16_t)DataType::Number || t == (uint16_t)DataType::Index ||
           t == (uint16_t)DataType::Uint32;
}

#ifndef SCALAR_ONLY
// Squared euclidean distance between a Vector value (up to 3 axes) and `last`, saturated so
// the comparison against the squared deadzone can never overflow (Docs: the delta trigger
// "Checks distance (euclidian for vectors)").
static uint32_t SubscriptionsVectorDist2(const FieldResult &fr, const int32_t *last) {
    uint8_t axes = (uint8_t)(fr.Descriptor.Size / 4);
    if (axes > 3) axes = 3;
    const uint8_t *data = (const uint8_t *)fr.Data;
    uint32_t sum = 0;
    for (uint8_t a = 0; a < axes; a++) {
        int32_t raw = 0;
        memcpy(&raw, data + (size_t)a * 4, 4);
        uint32_t x = (uint32_t)raw, y = (uint32_t)last[a];
        uint32_t d = x >= y ? x - y : y - x;
        if (d > 0xFFFFu) return 0xFFFFFFFFu; // >= 1.0 in 16.16: above any practical deadzone
        uint32_t t = d * d;
        if (sum > 0xFFFFFFFFu - t) return 0xFFFFFFFFu; // saturate, never wrap
        sum += t;
    }
    return sum;
}
#endif

static void EvaluateProviderTriggers(uint32_t nowMs) {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        ProviderEntry* e = &providerTable[i];
        if (e->requesterAddr == 0) continue;
        FieldResult fr = SubscriptionsGetField(e->sourceReg);
        if (!fr.Data) continue;
        uint8_t vlen = fr.Descriptor.Size;
        if (vlen > MAX_PAYLOAD_SIZE) vlen = MAX_PAYLOAD_SIZE;
        uint32_t elapsed = nowMs - e->lastSentMs;

        bool send = false;
        switch (e->trigger) {
        case TriggerType::Periodic:
            send = (e->periodMs > 0) && (elapsed >= e->periodMs);
            if (send) e->hash = Fnv1a((const uint8_t *)fr.Data, vlen);
            break;

        case TriggerType::OnChangePeriodic: {
            uint32_t h = Fnv1a((const uint8_t *)fr.Data, vlen);
            if (h != e->hash && elapsed >= e->minTimeMs) { send = true; e->hash = h; }
            else if (e->periodMs > 0 && elapsed >= e->periodMs) { send = true; e->hash = h; }
            break;
        }

        case TriggerType::OnChangeConfirm: {
            // Pure bit comparison; repeats until confirmed (hash updates on confirmation).
            uint32_t h = Fnv1a((const uint8_t *)fr.Data, vlen);
            send = (h != e->hash) && (elapsed >= e->minTimeMs);
            break;
        }

        case TriggerType::EdgeRising:
        case TriggerType::EdgeFalling:
        case TriggerType::EdgeAny: {
            bool cur = BlockMetaType(fr.Descriptor.FlagsAndType) == (uint16_t)DataType::Bool &&
                       ((const uint8_t *)fr.Data)[0] != 0;
            bool edge = (e->trigger == TriggerType::EdgeRising)  ? (cur && !e->lastBool)
                      : (e->trigger == TriggerType::EdgeFalling) ? (!cur && e->lastBool)
                                                                 : (cur != e->lastBool);
            e->lastBool = cur;
            if (edge) e->hash++; // counter; send on increase (not sooner than the retry interval)
            send = (e->hash != e->sentCounter) && (elapsed >= e->minTimeMs);
            if (send) e->sentCounter = e->hash;
            break;
        }

        case TriggerType::DeltaPeriodic: {
            if (SubscriptionsIsScalar(fr)) {
                // Scalar: Hash/Hashlike is the last SENT value; send once the change
                // magnitude reaches the deadzone (0 = any change).
                int32_t raw = 0;
                memcpy(&raw, fr.Data, 4);
                uint32_t a = (uint32_t)raw;
                uint32_t b = e->hash;
                uint32_t delta = a >= b ? a - b : b - a; // exact; unsigned wraps safely
                uint32_t dz = e->deadzone.Value > 0 ? (uint32_t)e->deadzone.Value : 0;
                if (delta >= dz && elapsed >= e->minTimeMs) {
                    send = true;
                    e->hash = a;
                } else if (e->periodMs > 0 && elapsed >= e->periodMs) {
                    send = true;
                    e->hash = a;
                }
                break;
            }
#ifndef SCALAR_ONLY
            if (BlockMetaType(fr.Descriptor.FlagsAndType) == (uint16_t)DataType::Vector &&
                fr.Descriptor.Size >= 4 && (fr.Descriptor.Size % 4) == 0) {
                // Vector: lastVec holds the last SENT vector; gate on the euclidean distance
                // (squared, to stay 32-bit and overflow-free).
                uint32_t dz = e->deadzone.Value > 0 ? (uint32_t)e->deadzone.Value : 0;
                uint32_t dz2 = dz > 0xFFFFu ? 0xFFFFFFFFu : dz * dz;
                if (SubscriptionsVectorDist2(fr, e->lastVec) >= dz2 && elapsed >= e->minTimeMs)
                    send = true;
                else if (e->periodMs > 0 && elapsed >= e->periodMs)
                    send = true;
                if (send) {
                    uint8_t n = fr.Descriptor.Size > 12 ? 12 : fr.Descriptor.Size;
                    memcpy(e->lastVec, fr.Data, n);
                    e->hash = SubscriptionsDeltaHash(fr, e->deadzone); // reported hashlike
                }
                break;
            }
#endif
            uint32_t h = SubscriptionsDeltaHash(fr, e->deadzone);
            if (h != e->hash && elapsed >= e->minTimeMs) { send = true; e->hash = h; }
            else if (e->periodMs > 0 && elapsed >= e->periodMs) { send = true; e->hash = h; }
            break;
        }

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
            if (e->trigger == TriggerType::OnChangeConfirm)
                e->hash = Fnv1a((const uint8_t *)fr.Data, vlen);
        }
#endif
        if (!applied) {
            uint8_t prio = SubscriptionsHighPriority(e->trigger) ? SUB_PRIORITY_HIGH : SUB_PRIORITY_LOW;
            PacketConstruct(&tx_frame, e->requesterAddr,
                            MakeService(ServiceType::Subscriptions, 0),
                            e->trid,
                            FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_REQACK,
                            (const uint8_t *)fr.Data, vlen, prio);
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
    uint8_t buf[1 + MAX_REQUESTER_SUBS * 26]; // entries without the TRID (non-persistent)
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
        *(uint32_t *)(buf + off) = (uint32_t)e->deadzone.Value; off += 4;
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
    for (uint8_t i = 0; i < count && off + 26 <= len; i++) {
        RequesterEntry* e = RequesterFindFree();
        if (!e) break;
        e->targetReg = *(uint32_t *)(buf + off); off += 4;
        e->sourceReg = *(uint32_t *)(buf + off); off += 4;
        e->providerAddr = *(uint16_t *)(buf + off); off += 2;
        e->trigger = (TriggerType)buf[off++];
        off += 3; // padding
        e->periodMs = *(uint32_t *)(buf + off); off += 4;
        e->minTimeMs = *(uint32_t *)(buf + off); off += 4;
        e->deadzone = Number::FromRaw(*(int32_t *)(buf + off)); off += 4;
        e->trid = SubscriptionsNextTrid();
        if (e->trid == 0) e->trid = TRID_SUB_BASE;
        e->active = true;
        e->registeredAtMs = DeviceStatus.UptimeMs;
        e->lastRegisteredMs = 0;
        e->lastValueMs = 0;
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
        p->deadzone = e->deadzone;
        p->lastBool = false;
        p->sentCounter = 0;
        return;
    }
#endif
    // Remote provider: send the subscription info (CID 1, fire and forget).
    uint8_t payload[26]; uint16_t off = 0;
    *(uint32_t *)(payload + off) = e->targetReg; off += 4;
    *(uint32_t *)(payload + off) = e->sourceReg; off += 4;
    *(uint16_t *)(payload + off) = DeviceStatus.ShortAddress; off += 2;
    payload[off++] = (uint8_t)e->trigger;
    payload[off++] = 0; payload[off++] = 0; payload[off++] = 0;
    *(uint32_t *)(payload + off) = e->periodMs; off += 4;
    *(uint32_t *)(payload + off) = e->minTimeMs; off += 4;
    *(uint32_t *)(payload + off) = (uint32_t)e->deadzone.Value; off += 4;
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

// Verifies each active requester subscription actually receives a value. Until the first
// value arrives the entry keeps re-registering with the provider (CID 1) on a short retry
// interval, so a dropped registration or slow provider node eventually gets woken up. The
// retries stop once a value is received or the initialization window expires.
static void RequesterInitCheck(uint32_t nowMs) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* e = &requesterTable[i];
        if (!e->active) continue;
        if (e->lastValueMs != 0) continue; // a value arrived; the subscription is live
        if (nowMs - e->registeredAtMs > SUB_INIT_WINDOW_MS) continue; // gave up
        if (nowMs - e->lastRegisteredMs < SUB_RETRY_MS) continue;
        e->lastRegisteredMs = nowMs;
        RegisterRequesterProvider(e);
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
            if (PayloadBytes(frame) < 4 + 4 + 2 + 1 + 4 + 4 + 4) break;

            uint16_t offset = 0;
            offset += 4; // targetReg (not used by provider)
            uint32_t sourceReg = *(uint32_t *)(frame.payload + offset); offset += 4;
            uint16_t requesterAddr = *(uint16_t *)(frame.payload + offset); offset += 2;
            TriggerType trigger = (TriggerType)frame.payload[offset++];
            offset += 3; // padding
            uint32_t periodMs = *(uint32_t *)(frame.payload + offset); offset += 4;
            uint32_t minTimeMs = *(uint32_t *)(frame.payload + offset); offset += 4;
            Number deadzone = Number::FromRaw(*(int32_t *)(frame.payload + offset)); offset += 4;

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
            e->deadzone = deadzone;
            e->lastSentMs = 0;
            e->hash = 0;
            e->lastBool = false;
            e->sentCounter = 0;
            e->lastVec[0] = e->lastVec[1] = e->lastVec[2] = 0;
            // Docs CID 1: the response to a change subscription is the CURRENT VALUE
            // (so the requester starts from a known state). Fall back to a 1-byte ack
            // when the source register does not resolve (yet).
            {
                FieldResult cur = SubscriptionsGetField(sourceReg);
                if (cur.Data) {
                    uint8_t vlen = cur.Descriptor.Size;
                    if (vlen > MAX_PAYLOAD_SIZE) vlen = MAX_PAYLOAD_SIZE;
                    SubReply(frame, (const uint8_t *)cur.Data, vlen);
                } else {
                    uint8_t resp = 1;
                    SubReply(frame, &resp, 1);
                }
            }
#endif
            break;
        }

        case 2: { // Get subscriptions (provider).
#ifdef USE_SUB_PROVIDE
            if (frame.payload_len == 0) {
                uint8_t count = 0;
                for (int i = 0; i < MAX_PROVIDER_SUBS; i++) if (providerTable[i].requesterAddr != 0) count++;
                uint8_t buf[1 + MAX_PROVIDER_SUBS * 32];
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
                    uint8_t buf[32];
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
                uint8_t buf[1 + MAX_REQUESTER_SUBS * 28];
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
                    uint8_t buf[28];
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
                        // Cancel the provider side with the shared TRID first (the frame's
                        // 8-bit TRID over the app link can't match a persisted 0xFAxx TRID).
                        RequesterEntry* removed = &requesterTable[index];
                        if (removed->providerAddr != 0) {
#ifdef USE_SUB_PROVIDE
                            if (removed->providerAddr == DeviceStatus.ShortAddress) {
                                ProviderEntry* p = ProviderFindByTrid(removed->trid);
                                if (p) ProviderClearEntry(p);
                            } else
#endif
                            {
                                PacketFrame cancel;
                                PacketConstruct(&cancel, removed->providerAddr,
                                                MakeService(ServiceType::Subscriptions, 1),
                                                removed->trid,
                                                FLAG_START | FLAG_STOP, nullptr, 0);
                                SendAndVerifyPacket(cancel);
                            }
                        }
                        // Delete + compact so the app's ordinal index stays in sync.
                        for (int i = index; i < MAX_REQUESTER_SUBS - 1; i++)
                            requesterTable[i] = requesterTable[i + 1];
                        RequesterClearEntry(&requesterTable[MAX_REQUESTER_SUBS - 1]);
                        SaveRequesterTable();
                        uint8_t resp = 1;
                        SubReply(frame, &resp, 1);
                    } else {
                        if (PayloadBytes(frame) < 1 + 2 + 2 + 4 + 4 + 1 + 4 + 4 + 4) break;
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
                        e->deadzone = Number::FromRaw(*(int32_t *)(frame.payload + offset)); offset += 4;
                        e->trid = frame.trid ? frame.trid : SubscriptionsNextTrid();
                        e->active = true;
                        e->registeredAtMs = DeviceStatus.UptimeMs;
                        e->lastRegisteredMs = 0;
                        e->lastValueMs = 0;
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
    }
}

void SubscriptionsTick(uint32_t nowMs) {
#ifdef USE_SUB_PROVIDE
    EvaluateProviderTriggers(nowMs);
#endif
#ifdef USE_SUB_REQUEST
    RequesterInitCheck(nowMs);
#endif
}