#pragma once

// Replies, management commands and the tick.
//
// Part of Core/Services/Subscriptions.h (included from there, after the class).

#include "Core/Services/SubscriptionsDefs.h"


// Shared reply for every subscription CID (docs: responses are packets with FLAG_TYPE).
inline void SubscriptionsService::SubReply(const PacketFrame &Frame, const uint8_t *Payload, uint16_t Len,
                     uint8_t extraFlags) {
    PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                    (uint8_t)(FLAG_TYPE | FLAG_START | FLAG_STOP | extraFlags), Payload, Len);
#ifdef USE_APP_INTERFACE
    if (Frame.IdSrc == 0xFFFE) {
        AppInterface.Send(TxFrame);
    } else {
        DispatchPacket(TxFrame);
    }
#else
    SendAndVerifyPacket(TxFrame);
#endif
}

#ifdef USE_SUB_REQUEST
// Cancels the provider side of a requester subscription (same-device: drop the local provider
// entry; remote: send 0401 with an empty payload = None/cancel). Requester-only: the provider
// side never cancels anything, so the DAS does not compile it.
inline void SubscriptionsService::SubscriptionsCancelProvider(uint16_t providerAddr, uint16_t Trid) {
#ifdef USE_SUB_PROVIDE
    uint8_t Net = (uint8_t)((DeviceStatus.ShortAddress >> 10) & 0x3F);
    if (NetQualifyLocal(providerAddr, Net) == DeviceStatus.ShortAddress) {
        ProviderEntry* p = ProviderFindByTrid(Trid);
        if (p) ProviderClearEntry(p);
        return;
    }
#endif
    PacketFrame Cancel;
    PacketConstruct(&Cancel, providerAddr, MakeService(ServiceType::Subscriptions, 1), Trid,
                    FLAG_START | FLAG_STOP, nullptr, 0);
    SendAndVerifyPacket(Cancel);
}
#endif

// Streams a subscription table (`entrySize`-byte entries, no count prefix) as FRAG fragments,
// copying each occupied entry straight into the fragment payload - no full-table buffer.
// `occupied` reports whether entry `i` is live (the provider and requester tables differ).
inline void SubscriptionsService::StreamSubscriptionTable(const PacketFrame &Frame, const uint8_t *Table, uint16_t Max,
                                    uint16_t entrySize, bool (*Occupied)(const uint8_t *, uint16_t)) {
    uint8_t Count = 0;
    for (uint16_t i = 0; i < Max; i++) if (Occupied(Table, i)) Count++;
    uint16_t Total = (uint16_t)((uint16_t)Count * entrySize);
    uint16_t Frags = (uint16_t)((Total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
    if (Frags == 0) Frags = 1;

    uint16_t Idx = 0;
    for (uint16_t f = 0; f < Frags; f++) {
        uint8_t *Dst = TxFrame.Payload + 4;
        uint16_t n = 0;
        while (Idx < Max && n + entrySize <= MAX_FRAG_CONTENT_SIZE) {
            if (!Occupied(Table, Idx)) { Idx++; continue; }
            memcpy(Dst + n, Table + (size_t)Idx * entrySize, entrySize);
            n += entrySize;
            Idx++;
        }
        SendFragFragment(Frame, f, Frags, n);
    }
}

#ifdef USE_SUB_PROVIDE
inline bool SubscriptionsService::ProviderEntryLive(const uint8_t *Table, uint16_t i) {
    return ((const ProviderEntry *)Table)[i].requesterAddr != 0;
}
// Streams the provider table (u8 count, then 32 B entries).
inline void SubscriptionsService::ProviderStreamTable(const PacketFrame &Frame) {
    StreamSubscriptionTable(Frame, (const uint8_t *)ProviderTable, MAX_PROVIDER_SUBS,
                            PROVIDER_ENTRY_WIRE_SIZE, ProviderEntryLive);
}
#endif

#ifdef USE_SUB_REQUEST
inline bool SubscriptionsService::RequesterEntryLive(const uint8_t *Table, uint16_t i) {
    return ((const RequesterEntry *)Table)[i].Active;
}
// Streams the requester table (u8 count, then 28 B entries).
inline void SubscriptionsService::RequesterStreamTable(const PacketFrame &Frame) {
    StreamSubscriptionTable(Frame, (const uint8_t *)RequesterTable, MAX_REQUESTER_SUBS,
                            REQUESTER_ENTRY_WIRE_SIZE, RequesterEntryLive);
}
#endif

__attribute__((noinline)) inline bool SubscriptionsService::Handle(const PacketFrame &Frame) {
#ifdef USE_SUB_REQUEST
    // A provider's reply to a requester-originated install ("Change subscription", CID 1)
    // echoes the requester's Subscriptions TRID, so the dispatcher routes it here by range.
    // It carries the source's current value: apply it through the normal value-update path so
    // a subscription starts from a known value instead of waiting for the next change/tick.
    // A locally generated management reply (id_src is us) is routed to the app link and never
    // reaches here.
    if ((Frame.Flags & FLAG_TYPE) && Frame.Trid >= TRID_SUB_BASE &&
        Frame.Trid <= TRID_SUB_MAX) {
        RequesterEntry* e = RequesterFindByTrid((uint16_t)Frame.Trid);
        // Stale reply after the entry was retargeted to a different provider: drop it. A
        // cancelled entry no longer resolves, so it falls through to the orphan handling.
        if (e && Frame.IdSrc != e->providerAddr) return true;
        HandleRequesterValueUpdate(Frame);
        return true;
    }
#endif
    uint8_t Cid = GetServiceCID(Frame.Cmd);

    switch (Cid) {
        case 0x00: {
#ifdef USE_SUB_REQUEST
            // Provider value update (response): apply + confirm (requester role).
            HandleRequesterValueUpdate(Frame);
#endif
#ifdef USE_SUB_PROVIDE
            // Requester confirmation (request): update the provider's hash.
            if (!(Frame.Flags & FLAG_TYPE))
                HandleProviderConfirmation(Frame);
#endif
            break;
        }

        case 0x01: { // Change subscription (inter-device, requester -> provider).
#ifdef USE_SUB_PROVIDE
            ProviderEntry* e = ProviderFindByTrid(Frame.Trid);
            bool Cancel = PayloadBytes(Frame) < SUB_TABLE_WIRE_SIZE ||
                          Frame.Payload[4] == (uint8_t)TriggerType::None;
            if (Cancel) {
                if (e) ProviderClearEntry(e);
                SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }

            SubscriptionTable t;
            memcpy((void *)&t, Frame.Payload, SUB_TABLE_WIRE_SIZE);
            // Reuse the entry found above (no second search). An explicit Set resets its trigger
            // state so a retargeted subscription sends its first value immediately.
            if (!e) e = ProviderFindFree();
            if (!e) { SubReply(Frame, nullptr, 0, FLAG_FAIL); break; }
            ProviderApply(e, Frame.Trid, Frame.IdSrc, t, true);
            // Docs CID 1: the response to a change subscription is the CURRENT VALUE (so the
            // requester starts from a known state). Fall back to a 1-byte ack when the source
            // register does not resolve (yet).
            {
                FieldResult Cur = SubscriptionsGetField(t.sourceReg);
                if (Cur.Data) {
                    uint8_t Vlen = Cur.Descriptor.Size;
                    if (Vlen > MAX_PAYLOAD_SIZE) Vlen = MAX_PAYLOAD_SIZE;
                    SubReply(Frame, (const uint8_t *)Cur.Data, Vlen);
                } else {
                    uint8_t Resp = 1;
                    SubReply(Frame, &Resp, 1);
                }
            }
#else
            SubReply(Frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        // Requester management commands (041x). A build without the requester role answers
        // them all with FAIL through one shared stub.
#if !defined(USE_SUB_REQUEST)
        case 0x10: case 0x11: case 0x12: case 0x13:
            SubReply(Frame, nullptr, 0, FLAG_FAIL);
            break;
#else
        case 0x10: { // Get subscriptions (requester).
            RequesterStreamTable(Frame);
            break;
        }

        case 0x11: { // Set subscription (requester). Trigger None = cancel.
            if (PayloadBytes(Frame) < 2 + 2 + SUB_TABLE_WIRE_SIZE + 4 + 4) {
                SubReply(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            uint16_t providerAddr = LoadUnaligned<uint16_t>(Frame.Payload);
            SubscriptionTable t;
            memcpy((void *)&t, Frame.Payload + 4, SUB_TABLE_WIRE_SIZE);
            uint32_t targetReg = LoadUnaligned<uint32_t>(Frame.Payload + 4 + SUB_TABLE_WIRE_SIZE);

            RequesterEntry* e = RequesterFindByTrid(Frame.Trid);
            if (t.Trigger == TriggerType::None) {
                if (e) {
                    uint16_t Provider = e->providerAddr;
                    RequesterRemove(e);
                    SaveRequesterTable();
                    SubscriptionsCancelProvider(Provider, Frame.Trid);
                }
                SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }

            // Reuse the entry found above; only allocate when it does not exist yet.
            if (!e) {
                e = RequesterAlloc(Frame.Trid);
                if (!e) { SubReply(Frame, nullptr, 0, FLAG_FAIL); break; }
            }
            e->providerAddr = providerAddr;
            e->Sub = t;
            e->targetReg = targetReg;
            e->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
            e->registeredAtMs = DeviceStatus.UptimeMs;
            e->lastRegisteredMs = 0;
            e->lastValueMs = 0;
            SaveRequesterTable();

            // Respond first (the app waits on the transaction ID), then register the provider
            // side (same-device or 0401 to the remote provider).
            SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
            RegisterRequesterProvider(e);
            break;
        }

        case 0x12: { // Recall all subscriptions (requester).
            LoadRequesterTable();
            SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
            break;
        }

        case 0x13: { // Save all subscriptions (requester).
            SaveRequesterTable();
            SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
            break;
        }
#endif // USE_SUB_REQUEST

        case 0x20: { // Get subscriptions (provider).
#ifdef USE_SUB_PROVIDE
            ProviderStreamTable(Frame);
#else
            SubReply(Frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        case 0x21: { // Set subscription (provider). Trigger None = cancel.
#ifdef USE_SUB_PROVIDE
            if (PayloadBytes(Frame) < 2 + 2 + SUB_TABLE_WIRE_SIZE + 4 + 4 + 4) {
                SubReply(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            uint16_t requesterAddr = LoadUnaligned<uint16_t>(Frame.Payload);
            SubscriptionTable t;
            memcpy((void *)&t, Frame.Payload + 4, SUB_TABLE_WIRE_SIZE);

            if (t.Trigger == TriggerType::None) {
                ProviderEntry* e = ProviderFindByTrid(Frame.Trid);
                if (e) ProviderClearEntry(e);
                SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }
            if (!ProviderInstall(Frame.Trid, requesterAddr, t, true)) {
                SubReply(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
#else
            SubReply(Frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        default:
            SubReply(Frame, nullptr, 0, FLAG_FAIL);
            break;
    }
    return true;
}

inline void SubscriptionsService::Tick(uint32_t nowMs) {
#ifdef USE_SUB_PROVIDE
    // Docs "Timeout 120s, renewed with new request": drop providers that stopped being renewed.
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        ProviderEntry* e = &ProviderTable[i];
        if (e->requesterAddr != 0 && e->Timeout != 0 && (int32_t)(nowMs - e->Timeout) >= 0)
            ProviderClearEntry(e);
    }
    EvaluateProviderTriggers(nowMs);
#endif
#ifdef USE_SUB_REQUEST
    bool requesterTableChanged = false;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* e = &RequesterTable[i];
        if (e->Active && e->Timeout != 0 && (int32_t)(nowMs - e->Timeout) >= 0) {
            uint16_t Provider = e->providerAddr;
            uint16_t Trid = e->Trid;
            RequesterRemove(e);          // compacts the table, shifting the tail left
            requesterTableChanged = true;
            SubscriptionsCancelProvider(Provider, Trid);
            i--;                         // re-check the entry shifted into slot i
        }
    }
    // Persist the removals: a timed-out entry must not be restored on the next boot.
    if (requesterTableChanged) SaveRequesterTable();
    RequesterInitCheck(nowMs);
    SubscriptionsReRegisterPending();
#endif
}
