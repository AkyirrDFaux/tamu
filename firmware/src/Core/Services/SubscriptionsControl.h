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

// Streams a table list as FRAG fragments (4-byte frag info + up to MAX_FRAG_CONTENT_SIZE bytes),
// the same shape the app reassembles for Register/Storage streams.
static void SubReplyStream(const PacketFrame &frame, const uint8_t *data, uint16_t len) {
    uint16_t frags = (uint16_t)((len + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
    if (frags == 0) frags = 1;

    // The shared `tx_frame` (not a local): the DAS's stack is tiny and a PacketFrame is 128 B.
    uint16_t off = 0;
    for (uint16_t f = 0; f < frags; f++) {
        uint16_t n = (uint16_t)((len - off < MAX_FRAG_CONTENT_SIZE) ? (len - off) : MAX_FRAG_CONTENT_SIZE);
        if (n > 0) memcpy(tx_frame.payload + 4, data + off, n);
        off += n;
        uint8_t flags = FLAG_TYPE | FLAG_FRAG;
        if (f == 0) flags |= FLAG_START;
        if (f == frags - 1) flags |= FLAG_STOP;
        WriteFragInfo(tx_frame.payload, f, frags);
        FinalizeReply(tx_frame, frame, flags, (uint16_t)(4 + n));
        DispatchPacket(tx_frame);
    }
}

// Cancels the provider side of a requester subscription (same-device: drop the local provider
// entry; remote: send 0401 with an empty payload = None/cancel).
static void SubscriptionsCancelProvider(uint16_t providerAddr, uint16_t trid) {
#ifdef USE_SUB_PROVIDE
    if (providerAddr == DeviceStatus.ShortAddress) {
        ProviderEntry* p = ProviderFindByTrid(trid);
        if (p) ProviderRemove(p);
        return;
    }
#endif
    PacketFrame cancel;
    PacketConstruct(&cancel, providerAddr, MakeService(ServiceType::Subscriptions, 1), trid,
                    FLAG_START | FLAG_STOP, nullptr, 0);
    SendAndVerifyPacket(cancel);
}

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
                if (e) ProviderRemove(e);
                SubReply(frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }

            SubscriptionTable t;
            SubTableDeserialize(frame.payload, 0, t);
            bool isNew = (e == nullptr);
            e = ProviderUpsert(frame.trid, frame.id_src);
            if (!e) break;
            e->requesterAddr = frame.id_src;
            e->sub = t;
            e->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
            if (isNew) {
                e->lastSentMs = 0;
                e->hash = 0;
                e->lastBool = false;
                e->sentCounter = 0;
                e->lastVec[0] = e->lastVec[1] = e->lastVec[2] = 0;
            }
            // Docs CID 1: the response to a change subscription is the CURRENT VALUE (so the
            // requester starts from a known state). Fall back to a 1-byte ack when the source
            // register does not resolve (yet).
            {
                FieldResult cur = SubscriptionsGetField(e->sub.sourceReg);
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

        case 0x10: { // Get subscriptions (requester).
#ifdef USE_SUB_REQUEST
            uint8_t buf[1 + MAX_REQUESTER_SUBS * 28];
            uint16_t off = 0;
            uint8_t count = 0;
            for (int i = 0; i < MAX_REQUESTER_SUBS; i++) if (requesterTable[i].active) count++;
            buf[off++] = count;
            for (int i = 0; i < MAX_REQUESTER_SUBS; i++)
                if (requesterTable[i].active)
                    off = RequesterEntrySerialize(buf, off, &requesterTable[i]);
            SubReplyStream(frame, buf, off);
#else
            SubReply(frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        case 0x11: { // Set subscription (requester). Trigger None = cancel.
#ifdef USE_SUB_REQUEST
            if (PayloadBytes(frame) < 2 + 2 + SUB_TABLE_WIRE_SIZE + 4 + 4) break;
            uint16_t providerAddr = LoadUnaligned<uint16_t>(frame.payload);
            SubscriptionTable t;
            SubTableDeserialize(frame.payload, 4, t);
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
#else
            SubReply(frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        case 0x12: { // Recall all subscriptions (requester).
#ifdef USE_SUB_REQUEST
            LoadRequesterTable();
            SubReply(frame, nullptr, 0, FLAG_SUCCESS);
#else
            SubReply(frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        case 0x13: { // Save all subscriptions (requester).
#ifdef USE_SUB_REQUEST
            SaveRequesterTable();
            SubReply(frame, nullptr, 0, FLAG_SUCCESS);
#else
            SubReply(frame, nullptr, 0, FLAG_FAIL);
#endif
            break;
        }

        case 0x20: { // Get subscriptions (provider).
#ifdef USE_SUB_PROVIDE
            uint8_t buf[1 + MAX_PROVIDER_SUBS * 32];
            uint16_t off = 0;
            uint8_t count = 0;
            for (int i = 0; i < MAX_PROVIDER_SUBS; i++) if (providerTable[i].requesterAddr != 0) count++;
            buf[off++] = count;
            for (int i = 0; i < MAX_PROVIDER_SUBS; i++)
                if (providerTable[i].requesterAddr != 0)
                    off = ProviderEntrySerialize(buf, off, &providerTable[i]);
            SubReplyStream(frame, buf, off);
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
            SubTableDeserialize(frame.payload, 4, t);

            ProviderEntry* e = ProviderFindByTrid(frame.trid);
            if (t.trigger == TriggerType::None) {
                if (e) ProviderRemove(e);
                SubReply(frame, nullptr, 0, FLAG_SUCCESS);
                break;
            }
            bool isNew = (e == nullptr);
            e = ProviderUpsert(frame.trid, requesterAddr);
            if (!e) break;
            e->requesterAddr = requesterAddr;
            e->sub = t;
            e->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
            if (isNew) {
                e->lastSentMs = 0;
                e->hash = 0;
                e->lastBool = false;
                e->sentCounter = 0;
                e->lastVec[0] = e->lastVec[1] = e->lastVec[2] = 0;
            }
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
            ProviderRemove(e);
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
