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
// The active provider table size. Defaults to 20; a RAM-constrained target overrides it with
// -D MAX_PROVIDER_SUBS=N (the DAS's 2 KB RAM needs only a couple of active providers). Kept as
// a build flag rather than a BOARD_X ifdef so Core/ stays board-agnostic (Docs/General
// architecture.md code rules).
#ifndef MAX_PROVIDER_SUBS
#define MAX_PROVIDER_SUBS 20
#endif

// Docs "Provider table entry" (32 B wire): requesterAddr, trid, the shared subscription
// table, last sent, hash/hashlike, timeout. Active until canceled; not persistent.
struct ProviderEntry {
    uint16_t requesterAddr = 0;  // 0 = invalid entry
    uint16_t trid = 0;
    SubscriptionTable sub;
    uint32_t lastSentMs = 0;
    uint32_t hash = 0;           // FNV-1a hash / edge counter / last value / subresolution
    uint32_t timeout = 0;        // local uptime deadline
    // Transient (not serialized): last boolean sample for edge detection and the last
    // counter value that was transmitted.
    bool lastBool = false;
    uint32_t sentCounter = 0;
    // Last SENT vector for a delta subscription on a Vector source (up to 3 axes); the
    // scalar delta reuses `hash` for the same purpose.
    int32_t lastVec[3] = {0, 0, 0};
};

static ProviderEntry providerTable[MAX_PROVIDER_SUBS];

// The wire prefix of the entry IS the struct prefix, so serialization is a plain copy of the
// first 32 bytes (Docs "Provider table entry"): requesterAddr, trid, table, lastSent, hash,
// timeout. The provider table is not ordered (only the requester table is TRID-sorted).
static_assert(offsetof(ProviderEntry, requesterAddr) == 0, "wire order: requesterAddr first");
static_assert(offsetof(ProviderEntry, trid) == 2, "wire order: trid after requesterAddr");
static_assert(offsetof(ProviderEntry, sub) == 4, "wire order: subscription table after trid");
static_assert(offsetof(ProviderEntry, lastSentMs) == 20, "wire order: lastSent after the table");
static_assert(offsetof(ProviderEntry, hash) == 24, "wire order: hash after lastSent");
static_assert(offsetof(ProviderEntry, timeout) == 28, "wire order: timeout after hash");
#define PROVIDER_ENTRY_WIRE_SIZE 32

// What makes a provider entry occupied (used by the shared table search).
static bool ProviderOccupied(const ProviderEntry &e) { return e.requesterAddr != 0; }

static ProviderEntry* ProviderFindByTrid(uint16_t trid) {
    return SubTableFindByTrid(providerTable, trid, ProviderOccupied);
}

static ProviderEntry* ProviderFindFree() {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++)
        if (providerTable[i].requesterAddr == 0) return &providerTable[i];
    return nullptr;
}

static void ProviderClearEntry(ProviderEntry* e) {
    *e = ProviderEntry{};
}

// Fills an already-resolved provider entry from a subscription table. `reset` forces a trigger
// state reset (an explicit Set); a new entry (requesterAddr 0) or a changed source register
// resets anyway, so a retargeted or re-set subscription cannot suppress its first send with a
// stale hash/last value from the previous target.
static void ProviderApply(ProviderEntry* e, uint16_t trid, uint16_t requesterAddr,
                          const SubscriptionTable &t, bool reset) {
    bool targetChanged = e->requesterAddr == 0 || e->sub.sourceReg != t.sourceReg;
    e->requesterAddr = requesterAddr;
    e->trid = trid;
    e->sub = t;
    e->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
    if (reset || targetChanged) {
        e->lastSentMs = 0;
        e->hash = 0;
        e->lastBool = false;
        e->sentCounter = 0;
        e->lastVec[0] = e->lastVec[1] = e->lastVec[2] = 0;
    }
}

// Installs (or updates) a provider entry from a subscription table. Returns the entry, or
// nullptr when the table is full. Shared by 0401/0421 and the same-device re-registration.
static ProviderEntry* ProviderInstall(uint16_t trid, uint16_t requesterAddr,
                                      const SubscriptionTable &t, bool reset = false) {
    ProviderEntry* e = ProviderFindByTrid(trid);
    if (!e) e = ProviderFindFree();
    if (!e) return nullptr;
    ProviderApply(e, trid, requesterAddr, t, reset);
    return e;
}

// The provider side of the requester's confirmation (CID 0 request): the payload is the
// FNV-1a hash of the value the requester received; confirm-required triggers stop here.
static void HandleProviderConfirmation(const PacketFrame &frame) {
    ProviderEntry* e = ProviderFindByTrid(frame.trid);
    if (!e) return;
    e->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs); // a confirmation renews the lease
    if (PayloadBytes(frame) >= 4) {
        uint32_t h;
        h = *reinterpret_cast<const uint32_t *>(frame.payload); // 4-byte aligned
        e->hash = h;
    }
}

// Per-value hashlike for the delta trigger: scalar -> the raw Number bits; vector ->
// the subresolution pack (Docs/Services/Subscriptions.md "Subresolution Vector"): 10 bits
// per axis (first 3 axes), each [5-bit above deadzone | 5-bit below], where "above" is a
// sign bit plus a 4-bit hash of the magnitude and "below" is the distance bucket 0..31.
static uint32_t SubscriptionsDeltaHash(const FieldResult &fr, Number deadzone) {
    uint16_t dtype = ValueInfoType(fr.Descriptor.Type);
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
                // av can exceed dz (the OnChange hash path calls this for any change; the
                // vector delta path calls it after the distance gate), so scale in 64 bits and
                // saturate at the top bucket.
                below5 = (uint32_t)(((uint64_t)(uint32_t)av * 31u) / (uint32_t)dz);
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
    uint16_t t = ValueInfoType(fr.Descriptor.Type);
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
        uint32_t elapsed = nowMs - e->lastSentMs;
        // A Periodic entry that is not due yet never sends, so skip it before resolving (and
        // possibly faulting on) its source register.
        if (e->sub.trigger == TriggerType::Periodic &&
            !(e->sub.periodMs > 0 && elapsed >= e->sub.periodMs))
            continue;
        FieldResult fr = SubscriptionsGetField(e->sub.sourceReg);
        if (!fr.Data) continue;
        uint8_t vlen = fr.Descriptor.Size;
        if (vlen > MAX_PAYLOAD_SIZE) vlen = MAX_PAYLOAD_SIZE;

        bool send = false;
#ifdef USE_SUB_REQUEST
        // OnChangeConfirm's current value hash (computed once), reused by the self-loopback
        // below instead of hashing the value a second time.
        uint32_t pendingHash = 0;
        bool havePendingHash = false;
#endif
        switch (e->sub.trigger) {
        case TriggerType::Periodic:
            send = true; // the not-due skip above already checked periodMs > 0 && elapsed >= periodMs
            break;

        case TriggerType::OnChangePeriodic: {
            const bool due = elapsed >= SubMinTime(e->sub);
            const bool periodic = e->sub.periodMs > 0 && elapsed >= e->sub.periodMs;
            if (due || periodic) { // hash only once a send is actually possible
                uint32_t h = Fnv1a((const uint8_t *)fr.Data, vlen);
                if (due && h != e->hash) { send = true; e->hash = h; }
                else if (periodic) { send = true; e->hash = h; }
            }
            break;
        }

        case TriggerType::OnChangeConfirm: {
            // Pure bit comparison; repeats until confirmed (hash updates on confirmation).
            if (elapsed >= SubMinTime(e->sub)) { // hash only after the gate
                uint32_t h = Fnv1a((const uint8_t *)fr.Data, vlen);
                send = (h != e->hash);
#ifdef USE_SUB_REQUEST
                pendingHash = h;
                havePendingHash = true;
#endif
            }
            break;
        }

        case TriggerType::EdgeRising:
        case TriggerType::EdgeFalling:
        case TriggerType::EdgeAny: {
            bool cur = ValueInfoType(fr.Descriptor.Type) == (uint16_t)DataType::Bool &&
                       ((const uint8_t *)fr.Data)[0] != 0;
            bool edge = (e->sub.trigger == TriggerType::EdgeRising)  ? (cur && !e->lastBool)
                      : (e->sub.trigger == TriggerType::EdgeFalling) ? (!cur && e->lastBool)
                                                                     : (cur != e->lastBool);
            e->lastBool = cur;
            if (edge) e->hash++; // counter; send on increase (not sooner than the retry interval)
            send = (e->hash != e->sentCounter) && (elapsed >= SubMinTime(e->sub));
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
                // The deadzone is Q16.16 for a Number source; an Index/Uint32 source is a
                // plain integer, so only its integer part applies (Value >> 16).
                int32_t dzRaw = ValueInfoType(fr.Descriptor.Type) == (uint16_t)DataType::Number
                                    ? e->sub.deadzone.Value
                                    : (e->sub.deadzone.Value >> 16);
                uint32_t dz = dzRaw > 0 ? (uint32_t)dzRaw : 0;
                // deadzone 0 means "any change": require a non-zero delta, not delta >= 0.
                bool changed = dz == 0 ? delta != 0 : delta >= dz;
                if (changed && elapsed >= SubMinTime(e->sub)) {
                    send = true;
                    e->hash = (uint32_t)raw;
                } else if (e->sub.periodMs > 0 && elapsed >= e->sub.periodMs) {
                    send = true;
                    e->hash = (uint32_t)raw;
                }
                break;
            }
#ifndef SCALAR_ONLY
            if (ValueInfoType(fr.Descriptor.Type) == (uint16_t)DataType::Vector &&
                fr.Descriptor.Size >= 4 && (fr.Descriptor.Size % 4) == 0) {
                // Vector: lastVec holds the last SENT vector; gate on the euclidean distance.
                //
                // Both sides live in Q8.8 (shifted right by 8): squaring a practical change or
                // deadzone in full Q16.16 overflows 32 bits, and saturating both sides at the
                // same ceiling silently capped every deadzone at 1.0 - a "huge" deadzone then
                // behaved like a tiny one, sending on any change over 1.0. Q8.8 covers +-256
                // units at 1/256 resolution; a deadzone below 1/256 still means "any change".
                uint32_t dz = e->sub.deadzone.Value > 0 ? ((uint32_t)e->sub.deadzone.Value >> 8) : 0;
                uint32_t dz2 = dz > 0xFFFFu ? 0xFFFFFFFFu : dz * dz;
                uint32_t dist2 = SubscriptionsVectorDist2(fr, e->lastVec);
                // deadzone 0 means "any change": require a non-zero distance.
                bool changed = dz == 0 ? dist2 != 0 : dist2 >= dz2;
                if (changed && elapsed >= SubMinTime(e->sub))
                    send = true;
                else if (e->sub.periodMs > 0 && elapsed >= e->sub.periodMs)
                    send = true;
                if (send) {
                    uint8_t n = fr.Descriptor.Size > 12 ? 12 : fr.Descriptor.Size;
                    memcpy(e->lastVec, fr.Data, n);
                    e->hash = SubscriptionsDeltaHash(fr, e->sub.deadzone); // reported hashlike
                }
                break;
            }
#endif
            uint32_t h = SubscriptionsDeltaHash(fr, e->sub.deadzone);
            if (h != e->hash && elapsed >= SubMinTime(e->sub)) { send = true; e->hash = h; }
            else if (e->sub.periodMs > 0 && elapsed >= e->sub.periodMs) { send = true; e->hash = h; }
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
            if (e->sub.trigger == TriggerType::OnChangeConfirm && havePendingHash)
                e->hash = pendingHash;
        }
#endif
        if (!applied) {
            uint8_t prio = SubscriptionsHighPriority(e->sub.trigger) ? SUB_PRIORITY_HIGH : SUB_PRIORITY_LOW;
            // Only OnChangeConfirm repeats until acknowledged; tagging every update with
            // FLAG_REQACK made the requester confirm triggers that never expect a reply.
            uint8_t flags = FLAG_TYPE | FLAG_START | FLAG_STOP;
            if (e->sub.trigger == TriggerType::OnChangeConfirm) flags |= FLAG_REQACK;
            PacketConstruct(&tx_frame, e->requesterAddr,
                            MakeService(ServiceType::Subscriptions, 0),
                            e->trid, flags,
                            (const uint8_t *)fr.Data, vlen, prio);
            SendAndVerifyPacket(tx_frame);
        }
    }
}
#endif // USE_SUB_PROVIDE
