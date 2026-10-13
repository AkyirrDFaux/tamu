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
    uint8_t Buf[1 + MAX_REQUESTER_SUBS * REQUESTER_FILE_ENTRY_SIZE];
    // Count and copy in one walk (the count byte is filled in last).
    uint16_t Off = 1;
    uint8_t Count = 0;
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* Entry = &RequesterTable[i];
        if (!Entry->Active) continue;
        memcpy(Buf + Off, Entry, REQUESTER_FILE_ENTRY_SIZE);
        Off += REQUESTER_FILE_ENTRY_SIZE;
        Count++;
    }
    // No subscriptions: remove the file rather than leaving an empty one behind (the app's
    // file list showed a `.SUBREQ` with nothing in it after the last cancel). Absence is the
    // same as an empty table on load. Drop a leftover staging file too, so an interrupted save
    // cannot leave `.SUBREQ~` behind.
    if (Count == 0) {
        Storage.DeleteFile(SubscriptionsRequesterFile);
        char Tmp[8];
        BackupTempName(SubscriptionsRequesterFile, Tmp);
        Storage.DeleteFile(Tmp);
        return;
    }
    Buf[0] = Count;
    // Skip the rewrite when the stored file already holds exactly these bytes. A set/cancel
    // rewrites `.SUBREQ` every time, so an unchanged re-set (or a redundant save-all) would
    // otherwise keep erasing/programming the same sector for nothing.
    uint32_t Existing = Storage.FileExists(SubscriptionsRequesterFile);
    if (Existing == Off) {
        // Compare against the file in entry-sized chunks - one small buffer instead of a second
        // full-size copy of the table - and stop at the first difference.
        uint8_t Chunk[REQUESTER_FILE_ENTRY_SIZE];
        bool Same = true;
        for (uint32_t Pos = 0; Pos < Off; Pos += sizeof(Chunk)) {
            uint16_t Want = (uint16_t)((Off - Pos > sizeof(Chunk)) ? sizeof(Chunk) : (Off - Pos));
            uint16_t Got = (uint16_t)Storage.ReadFromFile(SubscriptionsRequesterFile, Pos, Want, (char *)Chunk);
            if (Got != Want || memcmp(Chunk, Buf + Pos, Want) != 0) { Same = false; break; }
        }
        if (Same) return;
    }
    // Shared atomic staging (copy to the "~" name, then rename).
    WriteBackupFile(SubscriptionsRequesterFile, Buf, Off);
}

inline void SubscriptionsService::LoadRequesterTable() {
    // The whole file: a count byte plus every entry (1 + 16*24 = 385 B).
    uint8_t Buf[1 + MAX_REQUESTER_SUBS * REQUESTER_FILE_ENTRY_SIZE];
    uint16_t Len = Storage.ReadFromFile(SubscriptionsRequesterFile, 0, sizeof(Buf), (char*)Buf);
    if (Len < 1) {
        DeviceLog("SUB", "requester recall: no file (%u B)", (unsigned)Len);
        return;
    }
    uint8_t Count = Buf[0];
    // Validate before touching the table: a short or garbled file must not wipe a working table,
    // because the next save writes only the active entries and would make that loss permanent.
    if (Count > MAX_REQUESTER_SUBS || Len < 1 + (uint16_t)Count * REQUESTER_FILE_ENTRY_SIZE) {
        DeviceLog("SUB", "requester recall: bad file len=%u count=%u",
                  (unsigned)Len, (unsigned)Count);
        return;
    }

    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) RequesterClearEntry(&RequesterTable[i]);

    uint16_t Off = 1;
    uint16_t Loaded = 0;
    for (uint8_t i = 0; i < Count; i++) {
        uint16_t Trid = LoadUnaligned<uint16_t>(Buf + Off + 2);
        RequesterEntry* Entry = RequesterUpsert(Trid);
        if (!Entry) break;
        memcpy((void *)Entry, Buf + Off, REQUESTER_FILE_ENTRY_SIZE);
        Off += REQUESTER_FILE_ENTRY_SIZE;
        Loaded++;
        Entry->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
        Entry->RegisteredAtMs = DeviceStatus.UptimeMs;
        Entry->LastRegisteredMs = 0;
        Entry->LastValueMs = 0;
    }
    DeviceLog("SUB", "requester recall: %u of %u entries from %u B",
              (unsigned)Loaded, (unsigned)Count, (unsigned)Len);
}

// Re-registers the (non-persistent) provider side for a requester subscription so a
// restored table keeps pushing values after boot. Same-device providers get a direct
// provider-table entry; remote providers get a 0401 "Change subscription" packet.
inline void SubscriptionsService::RegisterRequesterProvider(RequesterEntry* Entry) {
    if (!Entry || !Entry->Active) return;

    Entry->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
    Entry->LastRegisteredMs = DeviceStatus.UptimeMs;

#ifdef USE_SUB_PROVIDE
    uint8_t Net = (uint8_t)((DeviceStatus.ShortAddress >> 10) & 0x3F);
    if (NetQualifyLocal(Entry->ProviderAddr, Net) == DeviceStatus.ShortAddress) {
        // Same-device provider: install it directly (shared with the wire paths).
        ProviderInstall(Entry->Trid, DeviceStatus.ShortAddress, Entry->Sub);
        return;
    }
#endif
    // Remote provider: send the subscription table (0401, fire and forget).
    uint8_t Payload[SUB_TABLE_WIRE_SIZE];
    memcpy(Payload, &Entry->Sub, SUB_TABLE_WIRE_SIZE);
    PacketConstruct(&TxFrame, Entry->ProviderAddr, MakeService(ServiceType::Subscriptions, (uint8_t)SubCid::Change),
                    Entry->Trid, FLAG_START | FLAG_STOP, Payload, SUB_TABLE_WIRE_SIZE);
    SendAndVerifyPacket(TxFrame);
}

inline void SubscriptionsService::ReRegister() {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        if (RequesterTable[i].Active)
            RegisterRequesterProvider(&RequesterTable[i]);
    }
}

// Re-pushes the requester entries whose provider is `addr`.
//
// A node's provider table is RAM-only ("active until cancelled"), so a node reboot wipes it
// while the core's requester entries still look active and healthy - the subscription then
// dies silently until the setup is re-applied. The node's add is TRID-keyed, so this replaces
// its entry rather than duplicating it.
inline void SubscriptionsService::ReRegisterForNode(uint16_t Addr) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry &Entry = RequesterTable[i];
        if (Entry.Active && Entry.ProviderAddr == Addr)
            RegisterRequesterProvider(&Entry);
    }
}

// Nodes that registered since the last tick. The discover handler only *requests* the re-push:
// it runs inside packet dispatch and must not be delayed by protocol traffic to a node that may
// not even be answering yet (a verified send blocks and retries). SubscriptionsTick does the
// sending from the main loop. A short address list (not a bitmask) keeps any 10-bit node id
// addressable; a duplicate request is dropped. The list is sized to the device registry's
// capacity (SNDB_MAX_ENTRIES) so a burst of assignments can never silently drop a re-push.
inline void SubscriptionsService::RequestReRegister(uint16_t Addr) {
    if (Addr == ADDR_INVALID) return;
    for (uint8_t i = 0; i < ReRegisterCount; i++)
        if (ReRegisterPending[i] == Addr) return; // already queued
    if (ReRegisterCount < KReRegisterMax)
        ReRegisterPending[ReRegisterCount++] = Addr;
}

// Called from SubscriptionsTick: performs the deferred re-pushes.
inline void SubscriptionsService::SubscriptionsReRegisterPending() {
    uint8_t Count = ReRegisterCount;
    ReRegisterCount = 0;
    for (uint8_t i = 0; i < Count; i++)
        ReRegisterForNode(ReRegisterPending[i]);
}

// Keeps the provider side alive: until the first value arrives the entry re-registers on a
// short retry interval; afterwards it renews the provider's 120 s lease on a slow keepalive
// (a value update only travels provider -> requester, so the requester must renew the
// provider explicitly - Docs "Timeout 120s, renewed with new request").
inline void SubscriptionsService::RequesterInitCheck(uint32_t NowMs) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* Entry = &RequesterTable[i];
        if (!Entry->Active) continue;
        uint32_t Interval;
        if (Entry->LastValueMs == 0) {
            if (NowMs - Entry->RegisteredAtMs > SUB_INIT_WINDOW_MS) continue; // gave up
            Interval = SUB_RETRY_MS;
        } else {
            Interval = SUB_KEEPALIVE_MS;
        }
        if (NowMs - Entry->LastRegisteredMs < Interval) continue;
        RegisterRequesterProvider(Entry);
    }
}
#endif // USE_SUB_REQUEST
