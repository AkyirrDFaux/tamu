#pragma once

// Replies, management commands and the tick.
//
// Part of Core/Services/Subscriptions.h (included from there).

#include "Core/Services/SubscriptionsDefs.h"


// Shared reply for every subscription CID (docs: responses are packets with FLAG_TYPE).
static void SubReply(const PacketFrame &frame, const uint8_t *payload, uint16_t len,
                     uint8_t extraFlags = 0) {
    PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                    (uint8_t)(FLAG_TYPE | FLAG_START | FLAG_STOP | extraFlags), payload, len);
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

#ifdef USE_SUB_REQUEST
// Cancels the provider side of a requester subscription (same-device: drop the local provider
// entry; remote: send 0401 with an empty payload = None/cancel). Requester-only: the provider
// side never cancels anything, so the DAS does not compile it.
static void SubscriptionsCancelProvider(uint16_t providerAddr, uint16_t trid) {
#ifdef USE_SUB_PROVIDE
    if (providerAddr == DeviceStatus.ShortAddress) {
        ProviderEntry* p = ProviderFindByTrid(trid);
        if (p) ProviderClearEntry(p);
        return;
    }
#endif
    PacketFrame cancel;
    PacketConstruct(&cancel, providerAddr, MakeService(ServiceType::Subscriptions, 1), trid,
                    FLAG_START | FLAG_STOP, nullptr, 0);
    SendAndVerifyPacket(cancel);
}
#endif

// Streams a subscription table (u8 count, then `entrySize`-byte entries) as FRAG fragments,
// copying each occupied entry straight into the fragment payload - no full-table buffer.
// `occupied` reports whether entry `i` is live (the provider and requester tables differ).
static void StreamSubscriptionTable(const PacketFrame &frame, const uint8_t *table, uint16_t max,
                                    uint16_t entrySize, bool (*occupied)(const uint8_t *, uint16_t)) {
    uint8_t count = 0;
    for (uint16_t i = 0; i < max; i++) if (occupied(table, i)) count++;
    uint16_t total = (uint16_t)(1 + (uint16_t)count * entrySize);
    uint16_t frags = (uint16_t)((total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
    if (frags == 0) frags = 1;

    bool countSent = false;
    uint16_t idx = 0;
    for (uint16_t f = 0; f < frags; f++) {
        uint8_t *dst = tx_frame.payload + 4;
        uint16_t n = 0;
        if (!countSent) { dst[n++] = count; countSent = true; }
        while (idx < max && n + entrySize <= MAX_FRAG_CONTENT_SIZE) {
            if (!occupied(table, idx)) { idx++; continue; }
            memcpy(dst + n, table + (size_t)idx * entrySize, entrySize);
            n += entrySize;
            idx++;
        }
        SendFragFragment(frame, f, frags, n);
    }
}

#ifdef USE_SUB_PROVIDE
static bool ProviderEntryLive(const uint8_t *table, uint16_t i) {
    return ((const ProviderEntry *)table)[i].requesterAddr != 0;
}
// Streams the provider table (u8 count, then 32 B entries).
static void ProviderStreamTable(const PacketFrame &frame) {
    StreamSubscriptionTable(frame, (const uint8_t *)providerTable, MAX_PROVIDER_SUBS,
                            PROVIDER_ENTRY_WIRE_SIZE, ProviderEntryLive);
}
#endif

#ifdef USE_SUB_REQUEST
static bool RequesterEntryLive(const uint8_t *table, uint16_t i) {
    return ((const RequesterEntry *)table)[i].active;
}
// Streams the requester table (u8 count, then 28 B entries).
static void RequesterStreamTable(const PacketFrame &frame) {
    StreamSubscriptionTable(frame, (const uint8_t *)requesterTable, MAX_REQUESTER_SUBS,
                            REQUESTER_ENTRY_WIRE_SIZE, RequesterEntryLive);
}
#endif

__attribute__((noinline)) static void HandleSubscriptions(const PacketFrame &frame) {
    uint8_t cid = GetServiceCID(frame.srv_tgt);

    switch (cid) {
        case 0x00: {
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

        case 0x01: { // Change subscription (inter-device, requester -> provider).
#ifdef USE_SUB_PROVIDE
            ProviderEntry* e = ProviderFindByTrid(frame.trid);
            bool cancel = PayloadBytes(frame) < SUB_TABLE_WIRE_SIZE ||
                          frame.payload[4] == (uint8_t)TriggerType::None;
            if (cancel) {
                if (e) ProviderClearEntry(e);
                SubReply(frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }

            SubscriptionTable t;
            memcpy((void *)&t, frame.payload, SUB_TABLE_WIRE_SIZE);
            e = ProviderInstall(frame.trid, frame.id_src, t);
            if (!e) break;
            // Docs CID 1: the response to a change subscription is the CURRENT VALUE (so the
            // requester starts from a known state). Fall back to a 1-byte ack when the source
            // register does not resolve (yet).
            {
                FieldResult cur = SubscriptionsGetField(t.sourceReg);
                if (cur.Data) {
                    uint8_t vlen = cur.Descriptor.Size;
                    if (vlen > MAX_PAYLOAD_SIZE) vlen = MAX_PAYLOAD_SIZE;
                    SubReply(frame, (const uint8_t *)cur.Data, vlen);
                } else {
                    uint8_t resp = 1;
                    SubReply(frame, &resp, 1);
                }
            }
#else
            SubReply(frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        // Requester management commands (041x). A build without the requester role answers
        // them all with FAIL through one shared stub.
#if !defined(USE_SUB_REQUEST)
        case 0x10: case 0x11: case 0x12: case 0x13:
            SubReply(frame, nullptr, 0, FLAG_FAIL);
            break;
#else
        case 0x10: { // Get subscriptions (requester).
            RequesterStreamTable(frame);
            break;
        }

        case 0x11: { // Set subscription (requester). Trigger None = cancel.
            if (PayloadBytes(frame) < 2 + 2 + SUB_TABLE_WIRE_SIZE + 4 + 4) break;
            uint16_t providerAddr = LoadUnaligned<uint16_t>(frame.payload);
            SubscriptionTable t;
            memcpy((void *)&t, frame.payload + 4, SUB_TABLE_WIRE_SIZE);
            uint32_t targetReg = LoadUnaligned<uint32_t>(frame.payload + 4 + SUB_TABLE_WIRE_SIZE);

            RequesterEntry* e = RequesterFindByTrid(frame.trid);
            if (t.trigger == TriggerType::None) {
                if (e) {
                    uint16_t provider = e->providerAddr;
                    RequesterRemove(e);
                    SaveRequesterTable();
                    SubscriptionsCancelProvider(provider, frame.trid);
                }
                SubReply(frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }

            e = RequesterUpsert(frame.trid);
            if (!e) break;
            e->providerAddr = providerAddr;
            e->sub = t;
            e->targetReg = targetReg;
            e->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
            e->registeredAtMs = DeviceStatus.UptimeMs;
            e->lastRegisteredMs = 0;
            e->lastValueMs = 0;
            SaveRequesterTable();

            // Respond first (the app waits on the transaction ID), then register the provider
            // side (same-device or 0401 to the remote provider).
            SubReply(frame, nullptr, 0, FLAG_SUCCESS);
            RegisterRequesterProvider(e);
            break;
        }

        case 0x12: { // Recall all subscriptions (requester).
            LoadRequesterTable();
            SubReply(frame, nullptr, 0, FLAG_SUCCESS);
            break;
        }

        case 0x13: { // Save all subscriptions (requester).
            SaveRequesterTable();
            SubReply(frame, nullptr, 0, FLAG_SUCCESS);
            break;
        }
#endif // USE_SUB_REQUEST

        case 0x20: { // Get subscriptions (provider).
#ifdef USE_SUB_PROVIDE
            ProviderStreamTable(frame);
#else
            SubReply(frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        case 0x21: { // Set subscription (provider). Trigger None = cancel.
#ifdef USE_SUB_PROVIDE
            if (PayloadBytes(frame) < 2 + 2 + SUB_TABLE_WIRE_SIZE + 4 + 4 + 4) break;
            uint16_t requesterAddr = LoadUnaligned<uint16_t>(frame.payload);
            SubscriptionTable t;
            memcpy((void *)&t, frame.payload + 4, SUB_TABLE_WIRE_SIZE);

            if (t.trigger == TriggerType::None) {
                ProviderEntry* e = ProviderFindByTrid(frame.trid);
                if (e) ProviderClearEntry(e);
                SubReply(frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }
            if (!ProviderInstall(frame.trid, requesterAddr, t)) break;
            SubReply(frame, nullptr, 0, FLAG_SUCCESS);
#else
            SubReply(frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        default:
            SubReply(frame, nullptr, 0, FLAG_FAIL);
            break;
    }
}

void SubscriptionsTick(uint32_t nowMs) {
#ifdef USE_SUB_PROVIDE
    // Docs "Timeout 120s, renewed with new request": drop providers that stopped being renewed.
    for (int i = 0; i < MAX_PROVIDER_SUBS; i++) {
        ProviderEntry* e = &providerTable[i];
        if (e->requesterAddr != 0 && e->timeout != 0 && (int32_t)(nowMs - e->timeout) >= 0)
            ProviderClearEntry(e);
    }
    EvaluateProviderTriggers(nowMs);
#endif
#ifdef USE_SUB_REQUEST
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* e = &requesterTable[i];
        if (e->active && e->timeout != 0 && (int32_t)(nowMs - e->timeout) >= 0) {
            uint16_t provider = e->providerAddr;
            uint16_t trid = e->trid;
            RequesterRemove(e);
            SubscriptionsCancelProvider(provider, trid);
        }
    }
    RequesterInitCheck(nowMs);
    SubscriptionsReRegisterPending();
#endif
}
