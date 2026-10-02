#pragma once

// Requester persistence and provider re-registration.
//
// Part of Core/Services/Subscriptions.h (included from there).

#include "Core/Services/SubscriptionsDefs.h"


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
        // Off starts at 1 (the count byte): every field here is misaligned.
        StoreUnaligned(buf + off, e->targetReg); off += 4;
        StoreUnaligned(buf + off, e->sourceReg); off += 4;
        StoreUnaligned(buf + off, e->providerAddr); off += 2;
        buf[off++] = (uint8_t)e->trigger;
        buf[off++] = 0; buf[off++] = 0; buf[off++] = 0;
        StoreUnaligned(buf + off, e->periodMs); off += 4;
        StoreUnaligned(buf + off, e->minTimeMs); off += 4;
        StoreUnaligned(buf + off, (uint32_t)e->deadzone.Value); off += 4;
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
        e->targetReg = LoadUnaligned<uint32_t>(buf + off); off += 4;
        e->sourceReg = LoadUnaligned<uint32_t>(buf + off); off += 4;
        e->providerAddr = LoadUnaligned<uint16_t>(buf + off); off += 2;
        e->trigger = (TriggerType)buf[off++];
        off += 3; // padding
        e->periodMs = LoadUnaligned<uint32_t>(buf + off); off += 4;
        e->minTimeMs = LoadUnaligned<uint32_t>(buf + off); off += 4;
        e->deadzone = Number::FromRaw(LoadUnaligned<int32_t>(buf + off)); off += 4;
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
    memcpy(payload + off, &e->targetReg, 4); off += 4;
    memcpy(payload + off, &e->sourceReg, 4); off += 4;
    StoreUnaligned(payload + off, DeviceStatus.ShortAddress); off += 2;
    payload[off++] = (uint8_t)e->trigger;
    payload[off++] = 0; payload[off++] = 0; payload[off++] = 0;
    memcpy(payload + off, &e->periodMs, 4); off += 4;
    memcpy(payload + off, &e->minTimeMs, 4); off += 4;
    memcpy(payload + off, &e->deadzone.Value, 4); off += 4;
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

// Re-pushes the requester entries whose provider is `addr`.
//
// A node's provider table is RAM-only ("active until cancelled"), so a node reboot wipes it
// while the core's requester entries still look active and healthy - the subscription then
// dies silently until the setup is re-applied. Retries exist, but they stop once the first
// value has arrived, which is exactly the state a running subscription is in. The node's add
// is TRID-keyed, so this replaces its entry rather than duplicating it.
void ReRegisterSubscriptionsForNode(uint16_t addr) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry &e = requesterTable[i];
        if (e.active && e.providerAddr == addr)
            RegisterRequesterProvider(&e);
    }
}

// Nodes that registered since the last tick, as a bitmask of addresses. The discover handler
// only *requests* the re-push: it runs inside packet dispatch and must not be delayed by
// protocol traffic to a node that may not even be answering yet (a verified send blocks and
// retries). SubscriptionsTick does the sending from the main loop.
static uint16_t s_reregisterPending = 0;

void SubscriptionsRequestReRegister(uint16_t addr) {
    if (addr > 0 && addr < 16)
        s_reregisterPending |= (uint16_t)(1u << addr);
}

// Called from SubscriptionsTick: performs the deferred re-pushes.
static void SubscriptionsReRegisterPending() {
    uint16_t pending = s_reregisterPending;
    s_reregisterPending = 0;
    while (pending) {
        uint16_t addr = (uint16_t)(pending & (~pending + 1)); // lowest set bit
        pending &= (uint16_t)(pending - 1);
        ReRegisterSubscriptionsForNode(addr);
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
