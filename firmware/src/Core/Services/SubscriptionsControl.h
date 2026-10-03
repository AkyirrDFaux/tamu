#pragma once

// Replies, management commands and the tick.
//
// Part of Core/Services/Subscriptions.h (included from there).

#include "Core/Services/SubscriptionsDefs.h"


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
            if (PayloadBytes(frame) == 0) {
                ProviderEntry* e = ProviderFindByTrid(frame.trid);
                if (e) ProviderClearEntry(e);
                uint8_t resp = 1;
                SubReply(frame, &resp, 1);
                break;
            }
            if (PayloadBytes(frame) < 4 + 4 + 2 + 1 + 4 + 4 + 4) break;

            uint16_t offset = 0;
            offset += 4; // targetReg (not used by provider)
            uint32_t sourceReg = 0;
            memcpy(&sourceReg, frame.payload + offset, 4); offset += 4;
            uint16_t requesterAddr = 0;
            memcpy(&requesterAddr, frame.payload + offset, 2); offset += 2;
            TriggerType trigger = (TriggerType)frame.payload[offset++];
            offset += 3; // padding
            uint32_t periodMs = 0;
            memcpy(&periodMs, frame.payload + offset, 4); offset += 4;
            uint32_t minTimeMs = 0;
            memcpy(&minTimeMs, frame.payload + offset, 4); offset += 4;
            int32_t deadzoneRaw = 0;
            memcpy(&deadzoneRaw, frame.payload + offset, 4); offset += 4;
            Number deadzone = Number::FromRaw(deadzoneRaw);

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
            if (PayloadBytes(frame) == 0) {
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
            if (PayloadBytes(frame) == 0) {
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
                    if (PayloadBytes(frame) == 1) {
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
                        memcpy(&e->providerAddr, frame.payload + offset, 2); offset += 2;
                        offset += 2; // payload TRID field: the frame TRID is authoritative
                        memcpy(&e->targetReg, frame.payload + offset, 4); offset += 4;
                        memcpy(&e->sourceReg, frame.payload + offset, 4); offset += 4;
                        e->trigger = (TriggerType)frame.payload[offset++];
                        offset += 3; // padding
                        memcpy(&e->periodMs, frame.payload + offset, 4); offset += 4;
                        memcpy(&e->minTimeMs, frame.payload + offset, 4); offset += 4;
                        e->deadzone = Number::FromRaw(LoadUnaligned<int32_t>(frame.payload + offset)); offset += 4;
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
                if (index < MAX_PROVIDER_SUBS && PayloadBytes(frame) == 1) {
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
    SubscriptionsReRegisterPending();
#endif
}
