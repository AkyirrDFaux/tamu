#pragma once

// Replies, management commands and the tick.
//
// Part of Core/Services/Subscriptions.h (included from there, after the class).

#include "Core/Services/SubscriptionsDefs.h"


// Shared reply for every subscription CID (docs: responses are packets with FLAG_TYPE).
inline void SubscriptionsService::SubReply(const PacketFrame &Frame, const uint8_t *Payload, uint16_t Len,
                     uint8_t ExtraFlags) {
    PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                    (uint8_t)(FLAG_TYPE | FLAG_START | FLAG_STOP | ExtraFlags), Payload, Len);
#ifdef USE_APP_INTERFACE
    // The dispatcher's targeting rule routes an app-bound reply back over the app link, so the
    // source address does not need to be inspected here.
    DispatchPacket(TxFrame);
#else
    SendAndVerifyPacket(TxFrame);
#endif
}

#ifdef USE_SUB_REQUEST
// Cancels the provider side of a requester subscription (same-device: drop the local provider
// entry; remote: send 0401 with an empty payload = None/cancel). Requester-only: the provider
// side never cancels anything, so the DAS does not compile it.
inline void SubscriptionsService::SubscriptionsCancelProvider(uint16_t ProviderAddr, uint16_t Trid) {
    // Re-qualified against the current net at use, like every other provider-address use: the
    // caller passes the address as stored, which carries the net it was set under.
    ProviderAddr = MakeId(DeviceStatus.NetId, (uint16_t)(ProviderAddr & 0x3FF));
#ifdef USE_SUB_PROVIDE
    if (ProviderAddr == DeviceStatus.ShortAddress) {
        ProviderEntry* Provider = ProviderFindByTrid(Trid);
        if (Provider) ProviderClearEntry(Provider);
        return;
    }
#endif
    PacketConstruct(&TxFrame, ProviderAddr, MakeService(ServiceType::Subscriptions, (uint8_t)SubCid::Change), Trid,
                    FLAG_START | FLAG_STOP, nullptr, 0);
    SendAndVerifyPacket(TxFrame);
}
#endif

// Streams a subscription table (`entrySize`-byte entries, no count prefix) as FRAG fragments,
// copying each occupied entry straight into the fragment payload - no full-table buffer.
// `occupied` reports whether an entry (already resolved to its address) is live; the entry
// stride is applied here, once, so the predicate never re-derives it.
inline void SubscriptionsService::StreamSubscriptionTable(const PacketFrame &Frame, const uint8_t *Table, uint16_t Max,
                                    uint16_t EntrySize, uint16_t Stride,
                                    bool (*Occupied)(const uint8_t *)) {
    uint8_t Count = 0;
    for (uint16_t i = 0; i < Max; i++) if (Occupied(Table + (size_t)i * Stride)) Count++;
    // Sized from the per-fragment packing, not the byte total: the loop below fills each fragment
    // with whole entries and stops before one would straddle the boundary, so 32-byte entries pack
    // three per fragment and ceil(Total/Capacity) under-counts the fragments the table needs, which
    // silently drops the tail of the reply.
    uint16_t PerFrag = (uint16_t)(MAX_FRAG_CONTENT_SIZE / EntrySize);
    if (PerFrag == 0) PerFrag = 1;
    uint16_t Frags = (uint16_t)((Count + PerFrag - 1) / PerFrag);
    if (Frags == 0) Frags = 1;

    uint16_t Idx = 0;
    for (uint16_t Frag = 0; Frag < Frags; Frag++) {
        uint8_t *Dst = TxFrame.Payload + 4;
        uint16_t Used = 0;
        while (Idx < Max && Used + EntrySize <= MAX_FRAG_CONTENT_SIZE) {
            const uint8_t *Entry = Table + (size_t)Idx * Stride;
            if (!Occupied(Entry)) { Idx++; continue; }
            memcpy(Dst + Used, Entry, EntrySize);
            Used += EntrySize;
            Idx++;
        }
        SendFragFragment(Frame, Frag, Frags, Used);
    }
}

#ifdef USE_SUB_PROVIDE
inline bool SubscriptionsService::ProviderEntryLive(const uint8_t *Entry) {
    return ((const ProviderEntry *)Entry)->RequesterAddr != 0;
}
// Streams the provider table (u8 count, then 32 B entries).
inline void SubscriptionsService::ProviderStreamTable(const PacketFrame &Frame) {
    StreamSubscriptionTable(Frame, (const uint8_t *)ProviderTable, MAX_PROVIDER_SUBS,
                            PROVIDER_ENTRY_WIRE_SIZE, sizeof(ProviderEntry), ProviderEntryLive);
}
#endif

#ifdef USE_SUB_REQUEST
inline bool SubscriptionsService::RequesterEntryLive(const uint8_t *Entry) {
    return ((const RequesterEntry *)Entry)->Active;
}
// Streams the requester table (u8 count, then 28 B entries).
inline void SubscriptionsService::RequesterStreamTable(const PacketFrame &Frame) {
    StreamSubscriptionTable(Frame, (const uint8_t *)RequesterTable, MAX_REQUESTER_SUBS,
                            REQUESTER_ENTRY_WIRE_SIZE, sizeof(RequesterEntry), RequesterEntryLive);
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
        RequesterEntry* Entry = RequesterFindByTrid((uint16_t)Frame.Trid);
        // Stale reply after the entry was retargeted to a different provider: drop it. A
        // cancelled entry no longer resolves, so it falls through to the orphan handling.
        if (Entry && Frame.IdSrc != RequesterProviderAddr(*Entry)) return true;
        HandleRequesterValueUpdate(Frame);
        return true;
    }
#endif
    uint8_t Cid = GetServiceCID(Frame.Cmd);

    switch (Cid) {
        case (uint8_t)SubCid::ValueUpdate: {
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

        case (uint8_t)SubCid::Change: { // Change subscription (inter-device, requester -> provider).
#ifdef USE_SUB_PROVIDE
            ProviderEntry* Entry = ProviderFindByTrid(Frame.Trid);
            bool Cancel = PayloadBytes(Frame) < SUB_TABLE_WIRE_SIZE ||
                          Frame.Payload[4] == (uint8_t)TriggerType::None;
            if (Cancel) {
                if (Entry) ProviderClearEntry(Entry);
                SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }

            SubscriptionTable Table;
            memcpy((void *)&Table, Frame.Payload, SUB_TABLE_WIRE_SIZE);
            // Reuse the entry found above (no second search). An explicit Set resets its trigger
            // state so a retargeted subscription sends its first value immediately.
            if (!Entry) Entry = ProviderFindFree();
            if (!Entry) { SubReply(Frame, nullptr, 0, FLAG_FAIL); break; }
            ProviderApply(Entry, Frame.Trid, Frame.IdSrc, Table, true);
            // Docs CID 1: the response to a change subscription is the CURRENT VALUE (so the
            // requester starts from a known state). Fall back to a 1-byte ack when the source
            // register does not resolve (yet).
            {
                FieldResult Cur = SubscriptionsGetField(Table.SourceReg);
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
        case (uint8_t)SubCid::RequesterGet:
        case (uint8_t)SubCid::RequesterSet:
        case (uint8_t)SubCid::RequesterRecall:
        case (uint8_t)SubCid::RequesterSave:
            SubReply(Frame, nullptr, 0, FLAG_FAIL);
            break;
#else
        case (uint8_t)SubCid::RequesterGet: { // Get subscriptions (requester).
            RequesterStreamTable(Frame);
            break;
        }

        case (uint8_t)SubCid::RequesterSet: { // Set subscription (requester). Trigger None = cancel.
            // A subscription is identified by a Subscriptions TRID (0x1000-0x1FFF) and both this
            // entry and the dispatcher route its value updates by that range, so an out-of-range
            // TRID would register an entry that looks healthy but never writes its target - at or
            // above TRID_APP_BASE the value update goes to the app link instead. Reject it here,
            // where the entry is accepted, rather than storing something that can never work.
            if (Frame.Trid < TRID_SUB_BASE || Frame.Trid > TRID_SUB_MAX) {
                SubReply(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            if (PayloadBytes(Frame) < 2 + 2 + SUB_TABLE_WIRE_SIZE + 4 + 4) {
                SubReply(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            uint16_t ProviderAddr = LoadUnaligned<uint16_t>(Frame.Payload);
            SubscriptionTable Table;
            memcpy((void *)&Table, Frame.Payload + 4, SUB_TABLE_WIRE_SIZE);
            uint32_t TargetReg = LoadUnaligned<uint32_t>(Frame.Payload + 4 + SUB_TABLE_WIRE_SIZE);

            RequesterEntry* Entry = RequesterFindByTrid(Frame.Trid);
            if (Table.Trigger == TriggerType::None) {
                bool Saved = true;
                if (Entry) {
                    uint16_t Provider = Entry->ProviderAddr;
                    RequesterRemove(Entry);
                    // An explicit cancel is what persists a removal, so this save writes the table
                    // alone and may drop the file once the last entry is gone.
                    Saved = SaveRequesterTable();
                    SubscriptionsCancelProvider(Provider, Frame.Trid);
                }
                SubReply(Frame, nullptr, 0, Saved ? FLAG_SUCCESS : FLAG_FAIL);
                break;
            }

            // Reuse the entry found above; only allocate when it does not exist yet.
            if (!Entry) {
                Entry = RequesterAlloc(Frame.Trid);
                if (!Entry) { SubReply(Frame, nullptr, 0, FLAG_FAIL); break; }
            }
            // The app addresses a local device with its net-0 shorthand (the SNDB stores
            // local devices with NetID 0), but the wire carries the fully qualified address:
            // a node adopts MakeId(localNet, shortId). Qualify it here so the stored provider
            // address matches the sender's IdSrc - otherwise every value update is dropped as
            // coming from a foreign address (SubscriptionsControl.h, value-update routing).
            Entry->ProviderAddr = NetQualifyLocal(ProviderAddr, DeviceStatus.NetId);
            Entry->Sub = Table;
            Entry->TargetReg = TargetReg;
            Entry->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
            Entry->Lease = SubRequesterLease{}; // unserved again: fast retry until a value arrives
            // Persist this set's own entry, but must not rewrite the file from the live table: an
            // entry another subscription lost to a lease expiry is not a deliberate removal, so it
            // has to survive in `.SUBREQ` for the next boot (and an empty table must not delete the
            // file at all). The write result decides the reply below: a failed persistence must not
            // read as a successful set.
            bool Saved = SaveRequesterTable(true);

            // Respond first (the app waits on the transaction ID), then register the provider
            // side (same-device or 0401 to the remote provider).
            SubReply(Frame, nullptr, 0, Saved ? FLAG_SUCCESS : FLAG_FAIL);
            RegisterRequesterProvider(Entry);
            break;
        }

        case (uint8_t)SubCid::RequesterRecall: { // Recall all subscriptions (requester).
            LoadRequesterTable();
            SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
            break;
        }

        case (uint8_t)SubCid::RequesterSave: { // Save all subscriptions (requester).
            // A deliberate Save All writes the table alone; its result is the reply.
            SubReply(Frame, nullptr, 0, SaveRequesterTable() ? FLAG_SUCCESS : FLAG_FAIL);
            break;
        }
#endif // USE_SUB_REQUEST

        case (uint8_t)SubCid::ProviderGet: { // Get subscriptions (provider).
#ifdef USE_SUB_PROVIDE
            ProviderStreamTable(Frame);
#else
            SubReply(Frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        case (uint8_t)SubCid::ProviderSet: { // Set subscription (provider). Trigger None = cancel.
#ifdef USE_SUB_PROVIDE
            if (PayloadBytes(Frame) < 2 + 2 + SUB_TABLE_WIRE_SIZE + 4 + 4 + 4) {
                SubReply(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            uint16_t RequesterAddr = LoadUnaligned<uint16_t>(Frame.Payload);
            SubscriptionTable Table;
            memcpy((void *)&Table, Frame.Payload + 4, SUB_TABLE_WIRE_SIZE);

            if (Table.Trigger == TriggerType::None) {
                ProviderEntry* Entry = ProviderFindByTrid(Frame.Trid);
                if (Entry) ProviderClearEntry(Entry);
                SubReply(Frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }
            if (!ProviderInstall(Frame.Trid, RequesterAddr, Table, true)) {
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

inline void SubscriptionsService::Tick(uint32_t NowMs) {
#ifdef USE_SUB_PROVIDE
    // Docs "Timeout 120s, renewed with new request": drop providers that stopped being renewed.
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        ProviderEntry* Entry = &ProviderTable[i];
        if (Entry->RequesterAddr != 0 && Entry->Timeout != 0 && (int32_t)(NowMs - Entry->Timeout) >= 0)
            ProviderClearEntry(Entry);
    }
    EvaluateProviderTriggers(NowMs);
#endif
#ifdef USE_SUB_REQUEST
    uint16_t Expired = 0;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* Entry = &RequesterTable[i];
        if (Entry->Active && Entry->Timeout != 0 && (int32_t)(NowMs - Entry->Timeout) >= 0) {
            uint16_t Provider = Entry->ProviderAddr;
            uint16_t Trid = Entry->Trid;
            RequesterRemove(Entry);          // compacts the table, shifting the tail left
            Expired++;
            SubscriptionsCancelProvider(Provider, Trid);
            i--;                         // re-check the entry shifted into slot i
        }
    }
    // Persist deliberate changes only. An expiry is a FAILURE - no value arrived, so the lease was
    // never renewed - and a failure must not delete anything: with the table empty the save removes
    // .SUBREQ, destroying the setup that a later boot would have restored. The entry is dropped
    // from the table so it stops retrying, but the file keeps the last known-good state; an
    // explicit cancel is what persists a removal.
    if (Expired)
        DeviceLog("SUB", "tick: %d requester entr(ies) expired, file kept",
                  (int)Expired);
    RequesterInitCheck(NowMs);
    SubscriptionsReRegisterPending();
#endif
}
