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
//
// `KeepFileEntries` is what a save driven by an unrelated change (a set, which persists only its
// own entry) passes: the file then keeps the entries it already holds that the live table no
// longer does. A lease expiry drops an entry from the table but must not lose it from the saved
// setup - the expiry is a FAILURE (no value ever arrived), not a removal - so the entry stays in
// `.SUBREQ` and the next boot restores it. Only a deliberate cancel or Save All rewrites the file
// from the table alone, and only those may remove the file when the table empties.
inline void SubscriptionsService::SaveRequesterTable(bool KeepFileEntries) {
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

    if (KeepFileEntries) {
        uint8_t File[1 + MAX_REQUESTER_SUBS * REQUESTER_FILE_ENTRY_SIZE];
        uint16_t Len = Storage.ReadFromFile(SubscriptionsRequesterFile, 0, sizeof(File), (char *)File);
        if (Len >= 1 && File[0] <= MAX_REQUESTER_SUBS &&
            Len >= (uint16_t)(1 + (uint16_t)File[0] * REQUESTER_FILE_ENTRY_SIZE)) {
            for (uint8_t i = 0; i < File[0] && Count < MAX_REQUESTER_SUBS; i++) {
                const uint8_t *Entry = File + 1 + (size_t)i * REQUESTER_FILE_ENTRY_SIZE;
                // Still in the live table: already copied above. A carried-over entry is appended;
                // the table proper is TRID-sorted but the file's order is only a restore input
                // (RequesterUpsert re-sorts on load).
                if (RequesterFindByTrid(LoadUnaligned<uint16_t>(Entry + 2))) continue;
                memcpy(Buf + Off, Entry, REQUESTER_FILE_ENTRY_SIZE);
                Off += REQUESTER_FILE_ENTRY_SIZE;
                Count++;
            }
        }
    }

    // No subscriptions at all: remove the file rather than leaving an empty one behind (the app's
    // file list showed a `.SUBREQ` with nothing in it after the last cancel). Absence is the
    // same as an empty table on load. Drop a leftover staging file too, so an interrupted save
    // cannot leave `.SUBREQ~` behind. A carry-over save only reaches here with an empty file,
    // so there is nothing to write and no deliberate removal to persist.
    if (Count == 0) {
        if (KeepFileEntries) return;
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

    // Re-qualified against the current net, not the net the entry was written under.
    uint16_t ProviderAddr = RequesterProviderAddr(*Entry);

    Entry->Timeout = SubTimeoutFrom(DeviceStatus.UptimeMs);
    Entry->LastRegisteredMs = DeviceStatus.UptimeMs;

#ifdef USE_SUB_PROVIDE
    if (ProviderAddr == DeviceStatus.ShortAddress) {
        // Same-device provider: install it directly (shared with the wire paths).
        ProviderInstall(Entry->Trid, DeviceStatus.ShortAddress, Entry->Sub);
        return;
    }
#endif
    // Remote provider: send the subscription table (0401, fire and forget).
    uint8_t Payload[SUB_TABLE_WIRE_SIZE];
    memcpy(Payload, &Entry->Sub, SUB_TABLE_WIRE_SIZE);
    PacketConstruct(&TxFrame, ProviderAddr, MakeService(ServiceType::Subscriptions, (uint8_t)SubCid::Change),
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
        // Match on the node's device field: the stored address carries the net the subscription
        // was set under, so comparing it whole would hide the node that just appeared after a net
        // change. The stored address is re-qualified to the node's current one below.
        if (!Entry.Active || (Entry.ProviderAddr & 0x3FF) != (Addr & 0x3FF)) continue;
        Entry.ProviderAddr = Addr;
        // The node has just appeared, so whatever it held is gone (its table is RAM-only): treat
        // these entries as un-served again, so they re-enter the fast retry window for the node's
        // whole start-up instead of sitting on the slow keepalive after a single push.
        Entry.LastValueMs = 0;
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
//
// The fast interval is keyed to the entry not being served, not to uptime: an un-served entry
// (restored at boot, or whose node just re-appeared and had LastValueMs cleared) keeps retrying
// until a value arrives, because its provider's table is RAM-only and may still be empty. The
// old uptime-keyed window collapsed to 60 s ten seconds after boot, so a node that came back
// later had no fast window left and its subscription stayed dead until the app re-applied the
// setup. The cost is that a requester whose provider never answers retries at SUB_RETRY_MS
// instead of once a minute; the entry still stops as soon as it is served.
inline void SubscriptionsService::RequesterInitCheck(uint32_t NowMs) {
    for (int i = 0; i < MAX_REQUESTER_SUBS; i++) {
        RequesterEntry* Entry = &RequesterTable[i];
        if (!Entry->Active) continue;
        uint32_t Interval = (Entry->LastValueMs == 0) ? SUB_RETRY_MS : SUB_KEEPALIVE_MS;
        if (NowMs - Entry->LastRegisteredMs < Interval) continue;
        RegisterRequesterProvider(Entry);
    }
}
#endif // USE_SUB_REQUEST
