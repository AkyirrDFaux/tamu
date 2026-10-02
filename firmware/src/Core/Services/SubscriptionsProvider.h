#pragma once

// Provider (USE_SUB_PROVIDE): the active provider table and triggers.
//
// Part of Core/Services/Subscriptions.h (included from there).

#include "Core/Services/SubscriptionsDefs.h"


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

// What makes a provider entry occupied (used by the shared table search).
static bool ProviderOccupied(const ProviderEntry &e) { return e.requesterAddr != 0; }

static ProviderEntry* ProviderFindByTrid(uint16_t trid) {
    return SubTableFindByTrid(providerTable, trid, ProviderOccupied);
}

static ProviderEntry* ProviderFindFree() {
    return SubTableFindFree(providerTable, ProviderOccupied);
}

static void ProviderClearEntry(ProviderEntry* e) {
    *e = ProviderEntry{};
}

// Serializes one provider entry in the CID 2 wire format (requesterAddr, trid, sourceReg,
// trigger + 3 pad, period, min, lastSent, hash, deadzone = 32 B).
static uint16_t ProviderEntrySerialize(uint8_t *buf, uint16_t off, const ProviderEntry *e) {
    // memcpy, not casts: the caller starts at off = 1 (the count byte), so every field here is
    // misaligned and an `lw`/`sw` on the RV32EC CH32 node would fault (see SubscriptionsControl).
    memcpy(buf + off, &e->requesterAddr, 2); off += 2;
    memcpy(buf + off, &e->trid, 2); off += 2;
    memcpy(buf + off, &e->sourceReg, 4); off += 4;
    buf[off++] = (uint8_t)e->trigger;
    buf[off++] = 0; buf[off++] = 0; buf[off++] = 0; // 24-bit padding
    memcpy(buf + off, &e->periodMs, 4); off += 4;
    memcpy(buf + off, &e->minTimeMs, 4); off += 4;
    memcpy(buf + off, &e->lastSentMs, 4); off += 4;
    memcpy(buf + off, &e->hash, 4); off += 4;
    memcpy(buf + off, &e->deadzone.Value, 4); off += 4;
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

// Magnitude of a change between two raw values, in raw units.
//
// Must be computed from the *signed* difference: subtracting the raw bit patterns (the usual
// unsigned trick) matches the value ordering only while both values sit on the same side of
// zero. A value that crosses zero - an accelerometer axis idling at ~0, a temperature passing
// 0 C - then looks like a change of nearly 2^32 and defeats every deadzone, so the delta send
// fires every minimum interval. Found on the rig: the vector delta "huge deadzone" test failed
// because the source's X axis (-0.38, Y 0.07) idles across zero.
static inline uint32_t SubscriptionsAbsDelta(int32_t now, int32_t last) {
    const int32_t diff = now - last; // wraps safely (-fwrapv); the unsigned negation below
    return (diff < 0) ? (uint32_t)(0u - (uint32_t)diff) : (uint32_t)diff; // handles INT32_MIN
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
        // Compared in Q8.8 (see the call site): +-256 value units at 1/256 resolution, which is
        // far beyond any real sensor and keeps the squares inside 32 bits.
        uint32_t d = SubscriptionsAbsDelta(raw, last[a]) >> 8;
        if (d > 0xFFFFu) return 0xFFFFFFFFu; // >= 256 units: above any meaningful deadzone
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
                uint32_t delta = SubscriptionsAbsDelta(raw, (int32_t)e->hash);
                uint32_t dz = e->deadzone.Value > 0 ? (uint32_t)e->deadzone.Value : 0;
                if (delta >= dz && elapsed >= e->minTimeMs) {
                    send = true;
                    e->hash = (uint32_t)raw;
                } else if (e->periodMs > 0 && elapsed >= e->periodMs) {
                    send = true;
                    e->hash = (uint32_t)raw;
                }
                break;
            }
#ifndef SCALAR_ONLY
            if (BlockMetaType(fr.Descriptor.FlagsAndType) == (uint16_t)DataType::Vector &&
                fr.Descriptor.Size >= 4 && (fr.Descriptor.Size % 4) == 0) {
                // Vector: lastVec holds the last SENT vector; gate on the euclidean distance.
                //
                // Both sides live in Q8.8 (shifted right by 8): squaring a practical change or
                // deadzone in full Q16.16 overflows 32 bits, and saturating both sides at the
                // same ceiling silently capped every deadzone at 1.0 - a "huge" deadzone then
                // behaved like a tiny one, sending on any change over 1.0. Q8.8 covers +-256
                // units at 1/256 resolution; a deadzone below 1/256 still means "any change".
                uint32_t dz = e->deadzone.Value > 0 ? ((uint32_t)e->deadzone.Value >> 8) : 0;
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
