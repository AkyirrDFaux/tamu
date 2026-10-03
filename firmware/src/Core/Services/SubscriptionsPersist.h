#pragma once

// Requester persistence and provider re-registration.
//
// Part of Core/Services/Subscriptions.h (included from there).

#include "Core/Services/SubscriptionsDefs.h"


// ===========================================================================
// Requester persistence + provider re-registration (USE_SUB_REQUEST).
// ===========================================================================
#ifdef USE_SUB_REQUEST
static const char* SubscriptionsRequesterFile = ".SUBREQ";

// Docs: "recalls values from a file, which is a 1:1 copy of the table except timeout value."
// Entry: the requester wire prefix minus the timeout = providerAddr(2) + trid(2) + table(16)
// + targetReg(4) = 24 B, so it is copied whole.

static void SaveRequesterTable() {
    uint8_t buf[1 + MAX_REQUESTER_SUBS * REQUESTER_FILE_ENTRY_SIZE];
    uint16_t off = 0;
    uint8_t count = 0;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) if (requesterTable[i].active) count++;
    buf[off++] = count;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* e = &requesterTable[i];
        if (!e->active) continue;
        memcpy(buf + off, e, REQUESTER_FILE_ENTRY_SIZE);
        off += REQUESTER_FILE_ENTRY_SIZE;
    }
    static const char tmp_name[8] = {'.','S','U','B','R','E','Q','~'};
    if (Storage.FileExists(tmp_name) != 0xFFFFFFFF)
        Storage.DeleteFile(tmp_name);
    if (!Storage.CreateFile(tmp_name, off)) return;
    if (!Storage.WriteToFile(tmp_name, 0, off, (const char*)buf)) { Storage.DeleteFile(tmp_name); return; }
    if (!Storage.RenameFile(tmp_name, SubscriptionsRequesterFile)) { Storage.DeleteFile(tmp_name); }
}

static void LoadRequesterTable() {
    // The whole file: a count byte plus every entry (1 + 16*24 = 385 B).
    uint8_t buf[1 + MAX_REQUESTER_SUBS * REQUESTER_FILE_ENTRY_SIZE];
    uint16_t len = Storage.ReadFromFile(SubscriptionsRequesterFile, 0, sizeof(buf), (char*)buf);
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) RequesterClearEntry(&requesterTable[i]);
    if (len == 0) return;

    uint16_t off = 0;
    uint8_t count = buf[off++];
    for (uint8_t i = 0; i < count && off + REQUESTER_FILE_ENTRY_SIZE <= len; i++) {
        uint16_t trid = LoadUnaligned<uint16_t>(buf + off + 2);
        RequesterEntry* e = RequesterUpsert(trid);
        if (!e) break;
        memcpy((void *)e, buf + off, REQUESTER_FILE_ENTRY_SIZE);
        off += REQUESTER_FILE_ENTRY_SIZE;
        e->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
        e->registeredAtMs = DeviceStatus.UptimeMs;
        e->lastRegisteredMs = 0;
        e->lastValueMs = 0;
    }
}

// Re-registers the (non-persistent) provider side for a requester subscription so a
// restored table keeps pushing values after boot. Same-device providers get a direct
// provider-table entry; remote providers get a 0401 "Change subscription" packet.
static void RegisterRequesterProvider(RequesterEntry* e) {
    if (!e || !e->active) return;

    e->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
    e->lastRegisteredMs = DeviceStatus.UptimeMs;

#ifdef USE_SUB_PROVIDE
    if (e->providerAddr == DeviceStatus.ShortAddress) {
        ProviderEntry* p = ProviderFindByTrid(e->trid);
        bool isNew = (p == nullptr);
        if (!p) p = ProviderFindFree();
        if (!p) return;
        p->requesterAddr = DeviceStatus.ShortAddress;
        p->trid = e->trid;
        p->sub = e->sub;
        p->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
        if (isNew) {
            p->lastSentMs = 0;
            p->hash = 0;
            p->lastBool = false;
            p->sentCounter = 0;
            p->lastVec[0] = p->lastVec[1] = p->lastVec[2] = 0;
        }
        return;
    }
#endif
    // Remote provider: send the subscription table (0401, fire and forget).
    uint8_t payload[SUB_TABLE_WIRE_SIZE];
    memcpy(payload, &e->sub, SUB_TABLE_WIRE_SIZE);
    PacketFrame req;
    PacketConstruct(&req, e->providerAddr, MakeService(ServiceType::Subscriptions, 1),
                    e->trid, FLAG_START | FLAG_STOP, payload, SUB_TABLE_WIRE_SIZE);
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
// dies silently until the setup is re-applied. The node's add is TRID-keyed, so this replaces
// its entry rather than duplicating it.
void ReRegisterSubscriptionsForNode(uint16_t addr) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry &e = requesterTable[i];
        if (e.active && e.providerAddr == addr)
            RegisterRequesterProvider(&e);
    }
}

// Nodes that registered since the last tick. The discover handler only *requests* the re-push:
// it runs inside packet dispatch and must not be delayed by protocol traffic to a node that may
// not even be answering yet (a verified send blocks and retries). SubscriptionsTick does the
// sending from the main loop. A short address list (not a bitmask) keeps any 10-bit node id
// addressable; a duplicate request is dropped.
static const uint8_t kReRegisterMax = 4;
static uint16_t s_reregisterPending[kReRegisterMax];
static uint8_t s_reregisterCount = 0;

void SubscriptionsRequestReRegister(uint16_t addr) {
    if (addr == ADDR_INVALID) return;
    for (uint8_t i = 0; i < s_reregisterCount; i++)
        if (s_reregisterPending[i] == addr) return; // already queued
    if (s_reregisterCount < kReRegisterMax)
        s_reregisterPending[s_reregisterCount++] = addr;
}

// Called from SubscriptionsTick: performs the deferred re-pushes.
static void SubscriptionsReRegisterPending() {
    uint8_t count = s_reregisterCount;
    s_reregisterCount = 0;
    for (uint8_t i = 0; i < count; i++)
        ReRegisterSubscriptionsForNode(s_reregisterPending[i]);
}

// Keeps the provider side alive: until the first value arrives the entry re-registers on a
// short retry interval; afterwards it renews the provider's 120 s lease on a slow keepalive
// (a value update only travels provider -> requester, so the requester must renew the
// provider explicitly - Docs "Timeout 120s, renewed with new request").
static void RequesterInitCheck(uint32_t nowMs) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* e = &requesterTable[i];
        if (!e->active) continue;
        uint32_t interval;
        if (e->lastValueMs == 0) {
            if (nowMs - e->registeredAtMs > SUB_INIT_WINDOW_MS) continue; // gave up
            interval = SUB_RETRY_MS;
        } else {
            interval = SUB_KEEPALIVE_MS;
        }
        if (nowMs - e->lastRegisteredMs < interval) continue;
        RegisterRequesterProvider(e);
    }
}
#endif // USE_SUB_REQUEST
