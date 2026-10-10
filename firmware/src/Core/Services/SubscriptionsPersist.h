#pragma once

// Requester persistence and provider re-registration.
//
// Part of Core/Services/Subscriptions.h (included from there, after the class).

#include "Core/Services/SubscriptionsDefs.h"


// ===========================================================================
// Requester persistence + provider re-registration (USE_SUB_REQUEST).
// ===========================================================================
#ifdef USE_SUB_REQUEST

// Docs: "recalls values from a file, which is a 1:1 copy of the table except timeout value."
// Entry: the requester wire prefix minus the timeout = providerAddr(2) + trid(2) + table(16)
// + targetReg(4) = 24 B, so it is copied whole.
inline void SubscriptionsService::SaveRequesterTable() {
    uint8_t buf[1 + MAX_REQUESTER_SUBS * REQUESTER_FILE_ENTRY_SIZE];
    // Count and copy in one walk (the count byte is filled in last).
    uint16_t off = 1;
    uint8_t count = 0;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* e = &RequesterTable[i];
        if (!e->active) continue;
        memcpy(buf + off, e, REQUESTER_FILE_ENTRY_SIZE);
        off += REQUESTER_FILE_ENTRY_SIZE;
        count++;
    }
    // No subscriptions: remove the file rather than leaving an empty one behind (the app's
    // file list showed a `.SUBREQ` with nothing in it after the last cancel). Absence is the
    // same as an empty table on load. Drop a leftover staging file too, so an interrupted save
    // cannot leave `.SUBREQ~` behind.
    if (count == 0) {
        Storage.DeleteFile(SubscriptionsRequesterFile);
        char tmp[8];
        BackupTempName(SubscriptionsRequesterFile, tmp);
        Storage.DeleteFile(tmp);
        return;
    }
    buf[0] = count;
    // Skip the rewrite when the stored file already holds exactly these bytes. A set/cancel
    // rewrites `.SUBREQ` every time, so an unchanged re-set (or a redundant save-all) would
    // otherwise keep erasing/programming the same sector for nothing.
    uint32_t existing = Storage.FileExists(SubscriptionsRequesterFile);
    if (existing == off) {
        uint8_t cur[1 + MAX_REQUESTER_SUBS * REQUESTER_FILE_ENTRY_SIZE];
        uint16_t curLen = (uint16_t)Storage.ReadFromFile(SubscriptionsRequesterFile, 0, off, (char *)cur);
        if (curLen == off && memcmp(cur, buf, off) == 0) return;
    }
    // Shared atomic staging (copy to the "~" name, then rename).
    WriteBackupFile(SubscriptionsRequesterFile, buf, off);
}

inline void SubscriptionsService::LoadRequesterTable() {
    // The whole file: a count byte plus every entry (1 + 16*24 = 385 B).
    uint8_t buf[1 + MAX_REQUESTER_SUBS * REQUESTER_FILE_ENTRY_SIZE];
    uint16_t len = Storage.ReadFromFile(SubscriptionsRequesterFile, 0, sizeof(buf), (char*)buf);
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) RequesterClearEntry(&RequesterTable[i]);
    if (len == 0) return;

    uint16_t off = 0;
    uint8_t count = buf[off++];
    for (uint8_t i = 0; i < count && off + REQUESTER_FILE_ENTRY_SIZE <= len; i++) {
        uint16_t Trid = LoadUnaligned<uint16_t>(buf + off + 2);
        RequesterEntry* e = RequesterUpsert(Trid);
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
inline void SubscriptionsService::RegisterRequesterProvider(RequesterEntry* e) {
    if (!e || !e->active) return;

    e->timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
    e->lastRegisteredMs = DeviceStatus.UptimeMs;

#ifdef USE_SUB_PROVIDE
    uint8_t net = (uint8_t)((DeviceStatus.ShortAddress >> 10) & 0x3F);
    if (NetQualifyLocal(e->providerAddr, net) == DeviceStatus.ShortAddress) {
        // Same-device provider: install it directly (shared with the wire paths).
        ProviderInstall(e->Trid, DeviceStatus.ShortAddress, e->sub);
        return;
    }
#endif
    // Remote provider: send the subscription table (0401, fire and forget).
    uint8_t Payload[SUB_TABLE_WIRE_SIZE];
    memcpy(Payload, &e->sub, SUB_TABLE_WIRE_SIZE);
    PacketFrame req;
    PacketConstruct(&req, e->providerAddr, MakeService(ServiceType::Subscriptions, 1),
                    e->Trid, FLAG_START | FLAG_STOP, Payload, SUB_TABLE_WIRE_SIZE);
    SendAndVerifyPacket(req);
}

inline void SubscriptionsService::ReRegister() {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (RequesterTable[i].active)
            RegisterRequesterProvider(&RequesterTable[i]);
    }
}

// Re-pushes the requester entries whose provider is `addr`.
//
// A node's provider table is RAM-only ("active until cancelled"), so a node reboot wipes it
// while the core's requester entries still look active and healthy - the subscription then
// dies silently until the setup is re-applied. The node's add is TRID-keyed, so this replaces
// its entry rather than duplicating it.
inline void SubscriptionsService::ReRegisterForNode(uint16_t addr) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry &e = RequesterTable[i];
        if (e.active && e.providerAddr == addr)
            RegisterRequesterProvider(&e);
    }
}

// Nodes that registered since the last tick. The discover handler only *requests* the re-push:
// it runs inside packet dispatch and must not be delayed by protocol traffic to a node that may
// not even be answering yet (a verified send blocks and retries). SubscriptionsTick does the
// sending from the main loop. A short address list (not a bitmask) keeps any 10-bit node id
// addressable; a duplicate request is dropped. The list is sized to the device registry's
// capacity (SNDB_MAX_ENTRIES) so a burst of assignments can never silently drop a re-push.
inline void SubscriptionsService::RequestReRegister(uint16_t addr) {
    if (addr == ADDR_INVALID) return;
    for (uint8_t i = 0; i < ReRegisterCount; i++)
        if (ReRegisterPending[i] == addr) return; // already queued
    if (ReRegisterCount < kReRegisterMax)
        ReRegisterPending[ReRegisterCount++] = addr;
}

// Called from SubscriptionsTick: performs the deferred re-pushes.
inline void SubscriptionsService::SubscriptionsReRegisterPending() {
    uint8_t count = ReRegisterCount;
    ReRegisterCount = 0;
    for (uint8_t i = 0; i < count; i++)
        ReRegisterForNode(ReRegisterPending[i]);
}

// Keeps the provider side alive: until the first value arrives the entry re-registers on a
// short retry interval; afterwards it renews the provider's 120 s lease on a slow keepalive
// (a value update only travels provider -> requester, so the requester must renew the
// provider explicitly - Docs "Timeout 120s, renewed with new request").
inline void SubscriptionsService::RequesterInitCheck(uint32_t nowMs) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* e = &RequesterTable[i];
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
