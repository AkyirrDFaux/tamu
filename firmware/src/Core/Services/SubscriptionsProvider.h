#pragma once

// Provider (USE_SUB_PROVIDE): the active provider table and triggers.
//
// Part of Core/Services/Subscriptions.h (included from there, after the class).

#include "Core/Services/SubscriptionsDefs.h"


// ===========================================================================
// Provider (USE_SUB_PROVIDE): active until canceled, not persistent. Updates checked
// from the main loop (SubscriptionsTick -> EvaluateProviderTriggers).
// ===========================================================================
#ifdef USE_SUB_PROVIDE

// What makes a provider entry occupied (used by the shared table search).
inline bool SubscriptionsService::ProviderOccupied(const ProviderEntry &Entry) { return Entry.RequesterAddr != 0; }

inline ProviderEntry* SubscriptionsService::ProviderFindByTrid(uint16_t Trid) {
    return SubTableFindByTrid(ProviderTable, Trid, ProviderOccupied);
}

inline ProviderEntry* SubscriptionsService::ProviderFindFree() {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++)
        if (ProviderTable[i].RequesterAddr == 0) return &ProviderTable[i];
    return nullptr;
}

inline void SubscriptionsService::ProviderClearEntry(ProviderEntry* Entry) {
    *Entry = ProviderEntry{};
}

// Fills an already-resolved provider entry from a subscription table. `reset` forces a trigger
// state reset (an explicit Set); a new entry (requesterAddr 0) or a changed source register
// resets anyway, so a retargeted or re-set subscription cannot suppress its first send with a
// stale hash/last value from the previous target.
inline void SubscriptionsService::ProviderApply(ProviderEntry* Entry, uint16_t Trid, uint16_t RequesterAddr,
                          const SubscriptionTable &Table, bool Reset) {
    bool TargetChanged = Entry->RequesterAddr == 0 || Entry->Sub.SourceReg != Table.SourceReg;
    Entry->RequesterAddr = RequesterAddr;
    Entry->Trid = Trid;
    Entry->Sub = Table;
    Entry->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
    if (Reset || TargetChanged) {
        Entry->LastSentMs = 0;
        Entry->Hash = 0;
        Entry->LastBool = false;
        Entry->EdgeDue = false;
        Entry->LastVec[0] = Entry->LastVec[1] = Entry->LastVec[2] = 0;
    }
}

// Installs (or updates) a provider entry from a subscription table. Returns the entry, or
// nullptr when the table is full. Shared by 0401/0421 and the same-device re-registration.
inline ProviderEntry* SubscriptionsService::ProviderInstall(uint16_t Trid, uint16_t RequesterAddr,
                                      const SubscriptionTable &Table, bool Reset) {
    ProviderEntry* Entry = ProviderFindByTrid(Trid);
    if (!Entry) Entry = ProviderFindFree();
    if (!Entry) return nullptr;
    ProviderApply(Entry, Trid, RequesterAddr, Table, Reset);
    return Entry;
}

// Lays out an edge-triggered value payload in the frame: the counter, and nothing else - for an
// edge trigger the counter IS the payload, and its parity carries the level (SubscriptionsEdge.h).
// One owner of the layout on the provider side, so the push cannot drift from what the requester
// parses. The construct call that follows passes nullptr as the payload, so it only fills the
// header and finalizes the CRC over the bytes already in the frame.
inline uint8_t SubscriptionsService::ProviderEdgePayload(uint32_t Counter) {
    SubEdgeCounterStore(TxFrame.Payload, Counter);
    return SUB_EDGE_COUNTER_SIZE;
}

// The provider side of the requester's confirmation (CID 0 request): the payload is the
// FNV-1a hash of the value the requester received; confirm-required triggers stop here.
inline void SubscriptionsService::HandleProviderConfirmation(const PacketFrame &Frame) {
    ProviderEntry* Entry = ProviderFindByTrid(Frame.Trid);
    if (!Entry) return;
    Entry->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs); // a confirmation renews the lease
    if (PayloadBytes(Frame) >= 4)
        Entry->Hash = *reinterpret_cast<const uint32_t *>(Frame.Payload); // 4-byte aligned
}

// Per-value hashlike for the delta trigger: scalar -> the raw Number bits; vector ->
// the subresolution pack (Docs/Services/Subscriptions.md "Subresolution Vector"): 10 bits
// per axis (first 3 axes), each [5-bit above deadzone | 5-bit below], where "above" is a
// sign bit plus a 4-bit hash of the magnitude and "below" is the distance bucket 0..31.
// The deadzone is taken in the value's own scalar type (Docs "Deadzone": its type follows
// the value): the Q16.16 raw bits for a Number/Vector, a plain integer for Index/Uint32 - a
// raw 32-bit scalar either way, so it is passed as `uint32_t` rather than a `Number`.
inline uint32_t SubscriptionsService::SubscriptionsDeltaHash(const FieldResult &Fr, uint32_t Deadzone) {
    uint16_t Dtype = ValueInfoType(Fr.Descriptor.Type);
    uint8_t Size = Fr.Descriptor.Size;
    const uint8_t *Data = (const uint8_t *)Fr.Data;
#ifndef SCALAR_ONLY
    if (Dtype == (uint16_t)DataType::Vector && Size >= 4 && (Size % 4) == 0) {
        uint8_t Axes = (uint8_t)(Size / 4);
        if (Axes > 3) Axes = 3;
        int32_t Dz = (int32_t)Deadzone;
        uint32_t packed = 0;
        for (uint8_t Axis = 0; Axis < Axes; Axis++) {
            int32_t Raw = 0;
            memcpy(&Raw, Data + (size_t)Axis * 4, 4);
            int32_t Av = Raw < 0 ? -Raw : Raw;
            uint32_t Above5 = ((Raw < 0 ? 1u : 0u) << 4) | ((((uint32_t)Av * 2654435761u) >> 28) & 0xF);
            uint32_t Below5 = 0;
            if (Dz > 0) {
                // av can exceed dz (the OnChange hash path calls this for any change; the
                // vector delta path calls it after the distance gate), so scale in 64 bits and
                // saturate at the top bucket.
                Below5 = (uint32_t)(((uint64_t)(uint32_t)Av * 31u) / (uint32_t)Dz);
                if (Below5 > 31) Below5 = 31;
            }
            packed |= ((Above5 << 5) | Below5) << (10 * Axis);
        }
        return packed;
    }
#endif
    if (Dtype == (uint16_t)DataType::Number && Size >= 4) {
        int32_t Raw = 0;
        memcpy(&Raw, Data, 4);
        return (uint32_t)Raw;
    }
    return Fnv1a(Data, Size);
}

// Magnitude of a change between two raw values, in raw units.
//
// Must be computed from the *signed* difference: subtracting the raw bit patterns (the usual
// unsigned trick) matches the value ordering only while both values sit on the same side of
// zero. A value that crosses zero - an accelerometer axis idling at ~0, a temperature passing
// 0 C - then looks like a change of nearly 2^32 and defeats every deadzone, so the delta send
// fires every minimum interval. Found on the rig: the vector delta "huge deadzone" test failed
// because the source's X axis (-0.38, Y 0.07) idles across zero.
inline uint32_t SubscriptionsService::SubscriptionsAbsDelta(int32_t NowMs, int32_t Last) {
    const int32_t Diff = NowMs - Last; // wraps safely (-fwrapv); the unsigned negation below
    return (Diff < 0) ? (uint32_t)(0u - (uint32_t)Diff) : (uint32_t)Diff; // handles INT32_MIN
}

// True for a 4-byte scalar numeric value. Delta subscriptions gate these on the change
// magnitude against the deadzone (Docs "Checks distance ... Last scalar value"); the
// vector path uses [SubscriptionsDeltaHash]'s subresolution pack instead.
inline bool SubscriptionsService::SubscriptionsIsScalar(const FieldResult &Fr) {
    if (Fr.Descriptor.Size < 4) return false;
    uint16_t TypeId = ValueInfoType(Fr.Descriptor.Type);
    return TypeId == (uint16_t)DataType::Number || TypeId == (uint16_t)DataType::Index ||
           TypeId == (uint16_t)DataType::Uint32;
}

#ifndef SCALAR_ONLY
// Squared euclidean distance between a Vector value (up to 3 axes) and `last`, saturated so
// the comparison against the squared deadzone can never overflow (Docs: the delta trigger
// "Checks distance (euclidian for vectors)").
inline uint32_t SubscriptionsService::SubscriptionsVectorDist2(const FieldResult &Fr, const int32_t *Last) {
    uint8_t Axes = (uint8_t)(Fr.Descriptor.Size / 4);
    if (Axes > 3) Axes = 3;
    const uint8_t *Data = (const uint8_t *)Fr.Data;
    uint32_t Sum = 0;
    for (uint8_t Axis = 0; Axis < Axes; Axis++) {
        int32_t Raw = 0;
        memcpy(&Raw, Data + (size_t)Axis * 4, 4);
        // Compared in Q8.8 (see the call site): +-256 value units at 1/256 resolution, which is
        // far beyond any real sensor and keeps the squares inside 32 bits.
        uint32_t Delta = SubscriptionsAbsDelta(Raw, Last[Axis]) >> 8;
        if (Delta > 0xFFFFu) return 0xFFFFFFFFu; // >= 256 units: above any meaningful deadzone
        uint32_t Sq = Delta * Delta;
        if (Sum > 0xFFFFFFFFu - Sq) return 0xFFFFFFFFu; // saturate, never wrap
        Sum += Sq;
    }
    return Sum;
}
#endif

inline void SubscriptionsService::EvaluateProviderTriggers(uint32_t NowMs) {
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        ProviderEntry* Entry = &ProviderTable[i];
        if (Entry->RequesterAddr == 0) continue;
        uint32_t Elapsed = NowMs - Entry->LastSentMs;
        // A Periodic entry that is not due yet never sends, so skip it before resolving (and
        // possibly faulting on) its source register.
        if (Entry->Sub.Trigger == TriggerType::Periodic &&
            !(Entry->Sub.PeriodMs > 0 && Elapsed >= Entry->Sub.PeriodMs))
            continue;
        FieldResult Fr = SubscriptionsGetField(Entry->Sub.SourceReg);
        if (!Fr.Data) continue;
        uint8_t Vlen = Fr.Descriptor.Size;
        // An edge-triggered update carries the counter alone (SubscriptionsEdge.h), so its payload is
        // two bytes whatever the source's width; the clamp is about the value bytes of every other
        // trigger.
        const bool Edge = SubTriggerIsEdge(Entry->Sub.Trigger);
        if (!Edge && Vlen > MAX_PAYLOAD_SIZE) Vlen = MAX_PAYLOAD_SIZE;

        bool Send = false;
#ifdef USE_SUB_REQUEST
        // OnChangeConfirm's current value hash (computed once), reused by the self-loopback
        // below instead of hashing the value a second time.
        uint32_t PendingHash = 0;
        bool HavePendingHash = false;
#endif
        switch (Entry->Sub.Trigger) {
        case TriggerType::Periodic:
            Send = true; // the not-due skip above already checked periodMs > 0 && elapsed >= periodMs
            break;

        case TriggerType::OnChangePeriodic: {
            const bool Due = Elapsed >= SubMinTime(Entry->Sub);
            const bool Periodic = Entry->Sub.PeriodMs > 0 && Elapsed >= Entry->Sub.PeriodMs;
            if (Due || Periodic) { // hash only once a send is actually possible
                uint32_t Hash = Fnv1a((const uint8_t *)Fr.Data, Vlen);
                if (Due && Hash != Entry->Hash) { Send = true; Entry->Hash = Hash; }
                else if (Periodic) { Send = true; Entry->Hash = Hash; }
            }
            break;
        }

        case TriggerType::OnChangeConfirm: {
            // Pure bit comparison; repeats until confirmed (hash updates on confirmation).
            if (Elapsed >= SubMinTime(Entry->Sub)) { // hash only after the gate
                uint32_t Hash = Fnv1a((const uint8_t *)Fr.Data, Vlen);
                Send = (Hash != Entry->Hash);
#ifdef USE_SUB_REQUEST
                PendingHash = Hash;
                HavePendingHash = true;
#endif
            }
            break;
        }

        case TriggerType::EdgeRising:
        case TriggerType::EdgeFalling:
        case TriggerType::EdgeAny: {
            bool Cur = ValueInfoType(Fr.Descriptor.Type) == (uint16_t)DataType::Bool &&
                       ((const uint8_t *)Fr.Data)[0] != 0;
            // The counter counts EVERY level change, so its parity is the level (SubscriptionsEdge.h);
            // the trigger only decides which of those changes are sent. Counting just the trigger's
            // own direction would leave a rising-only counter odd after the release, and the
            // requester would read "still pressed" from a lifted switch.
            const SubEdgeStep Step = SubEdgeClassify(Entry->Sub.Trigger, Cur, Entry->LastBool);
            Entry->LastBool = Cur;
            if (Step.Counted) {
                Entry->Hash++; // counter; send on an edge, not sooner than the retry interval
                if (Step.Due) Entry->EdgeDue = true;
            }
            Send = Entry->EdgeDue && (Elapsed >= SubMinTime(Entry->Sub));
            if (Send) Entry->EdgeDue = false;
            break;
        }

        case TriggerType::DeltaPeriodic: {
            if (SubscriptionsIsScalar(Fr)) {
                // Scalar: Hash/Hashlike is the last SENT value; send once the change
                // magnitude reaches the deadzone (0 = any change).
                int32_t Raw = 0;
                memcpy(&Raw, Fr.Data, 4);
                uint32_t Delta = SubscriptionsAbsDelta(Raw, (int32_t)Entry->Hash);
                // The deadzone is stored in the value's own scalar type (Docs "Deadzone"): the
                // Q16.16 raw bits for a Number source, a plain integer for Index/Uint32, so the
                // stored 32 bits compare as-is against the raw delta.
                int32_t DzRaw = Entry->Sub.Deadzone.Value;
                uint32_t Dz = DzRaw > 0 ? (uint32_t)DzRaw : 0;
                // deadzone 0 means "any change": require a non-zero delta, not delta >= 0.
                bool Changed = Dz == 0 ? Delta != 0 : Delta >= Dz;
                if (Changed && Elapsed >= SubMinTime(Entry->Sub)) {
                    Send = true;
                    Entry->Hash = (uint32_t)Raw;
                } else if (Entry->Sub.PeriodMs > 0 && Elapsed >= Entry->Sub.PeriodMs) {
                    Send = true;
                    Entry->Hash = (uint32_t)Raw;
                }
                break;
            }
#ifndef SCALAR_ONLY
            if (ValueInfoType(Fr.Descriptor.Type) == (uint16_t)DataType::Vector &&
                Fr.Descriptor.Size >= 4 && (Fr.Descriptor.Size % 4) == 0) {
                // Vector: lastVec holds the last SENT vector; gate on the euclidean distance.
                //
                // Both sides live in Q8.8 (shifted right by 8): squaring a practical change or
                // deadzone in full Q16.16 overflows 32 bits, and saturating both sides at the
                // same ceiling silently capped every deadzone at 1.0 - a "huge" deadzone then
                // behaved like a tiny one, sending on any change over 1.0. Q8.8 covers +-256
                // units at 1/256 resolution; a deadzone below 1/256 still means "any change".
                uint32_t Dz = Entry->Sub.Deadzone.Value > 0 ? ((uint32_t)Entry->Sub.Deadzone.Value >> 8) : 0;
                uint32_t Dz2 = Dz > 0xFFFFu ? 0xFFFFFFFFu : Dz * Dz;
                uint32_t Dist2 = SubscriptionsVectorDist2(Fr, Entry->LastVec);
                // deadzone 0 means "any change": require a non-zero distance.
                bool Changed = Dz == 0 ? Dist2 != 0 : Dist2 >= Dz2;
                if (Changed && Elapsed >= SubMinTime(Entry->Sub))
                    Send = true;
                else if (Entry->Sub.PeriodMs > 0 && Elapsed >= Entry->Sub.PeriodMs)
                    Send = true;
                if (Send) {
                    uint8_t Count = Fr.Descriptor.Size > 12 ? 12 : Fr.Descriptor.Size;
                    memcpy(Entry->LastVec, Fr.Data, Count);
                    Entry->Hash = SubscriptionsDeltaHash(Fr, (uint32_t)Entry->Sub.Deadzone.Value); // reported hashlike
                }
                break;
            }
#endif
            uint32_t Hash = SubscriptionsDeltaHash(Fr, (uint32_t)Entry->Sub.Deadzone.Value);
            if (Hash != Entry->Hash && Elapsed >= SubMinTime(Entry->Sub)) { Send = true; Entry->Hash = Hash; }
            else if (Entry->Sub.PeriodMs > 0 && Elapsed >= Entry->Sub.PeriodMs) { Send = true; Entry->Hash = Hash; }
            break;
        }

        default:
            break;
        }
        if (!Send) continue;

        Entry->LastSentMs = NowMs;

        // Self-loopback (provider and requester on the same device): apply locally; a
        // confirm-required trigger is confirmed immediately (no bus round-trip).
        bool Applied = false;
#ifdef USE_SUB_REQUEST
        if (Entry->RequesterAddr == DeviceStatus.ShortAddress) {
            RequesterEntry* Requester = RequesterFindByTrid(Entry->Trid);
            if (Requester) {
                // An edge send carries the counter on the wire; locally there is no wire, so the
                // same counter goes in directly - the row's accounting must not depend on whether
                // its provider happens to be on another node.
                if (SubTriggerIsEdge(Entry->Sub.Trigger))
                    ApplyRequesterEdge(Requester, (uint16_t)Entry->Hash);
                else
                    ApplyRequesterValue(Requester, (const uint8_t *)Fr.Data, Vlen, false);
                Applied = true;
            }
            if (Entry->Sub.Trigger == TriggerType::OnChangeConfirm && HavePendingHash)
                Entry->Hash = PendingHash;
        }
#endif
        if (!Applied) {
            uint8_t Prio = SubscriptionsHighPriority(Entry->Sub.Trigger) ? PRIORITY_SUB_HIGH : PRIORITY_SUB_LOW;
            // Only OnChangeConfirm repeats until acknowledged; tagging every update with
            // FLAG_REQACK made the requester confirm triggers that never expect a reply.
            uint8_t Flags = FLAG_TYPE | FLAG_START | FLAG_STOP;
            if (Entry->Sub.Trigger == TriggerType::OnChangeConfirm) Flags |= FLAG_REQACK;
            const uint8_t *Src = (const uint8_t *)Fr.Data;
            uint8_t Len = Vlen;
            if (Edge) {
                // Docs: an edge-triggered value update is the edge counter (u16 little-endian,
                // SubscriptionsEdge.h) and nothing else.
                Len = ProviderEdgePayload(Entry->Hash);
                Src = nullptr; // already laid out in the frame
            }
            PacketConstruct(&TxFrame, Entry->RequesterAddr,
                            MakeService(ServiceType::Subscriptions, (uint8_t)SubCid::ValueUpdate),
                            Entry->Trid, Flags, Src, Len, Prio);
            SendAndVerifyPacket(TxFrame);
        }
    }
}
#endif // USE_SUB_PROVIDE
