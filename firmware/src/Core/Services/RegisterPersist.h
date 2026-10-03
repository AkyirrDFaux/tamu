#pragma once

// CID 3/4 save+recall and the 0x10-0x13 dynamic management.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 4/5: Save/Recall for Static =====
//
// The static memory's persistent half is one flat space (the board's `staticPer`, Docs/Services/
// Register.md "Persistence"), mirrored 1:1 to `.SV`. The space already holds the live System
// fields and every static block's persistent half, so a save is a single copy out and a recall a
// single copy in; targeted saves/recalls are the app's job (a direct file write at the field's
// offset).

static_assert(sizeof(staticPer) <= MEMORY_BACKUP_CAP,
              "static persistent space must fit the backup buffer");

static bool StaticSaveAll() {
    // Snapshot the live System fields into the space (the only persistent values not already
    // written through it), then mirror the whole space to .SV in one write.
#ifdef TYPE_CORE
    staticPer.system.NetId = DeviceStatus.NetId;
#endif
    return WriteBackupFile(StaticValuesName(), (const uint8_t *)&staticPer, sizeof(staticPer));
}

static void StaticRecallAll() {
    uint8_t buf[sizeof(staticPer)];
    uint16_t cnt = ReadBackupFile(StaticValuesName(), buf, sizeof(buf));
    if (cnt < sizeof(staticPer)) return; // no (or short) backup: keep the compiled-in defaults
    memcpy(&staticPer, buf, sizeof(staticPer));
#ifdef TYPE_CORE
    DeviceStatus.NetId = staticPer.system.NetId;
#endif
}

#ifdef USE_DYNAMIC_BLOCKS
// Saves every dynamic block to its DT_/DV_ files (the dynamic half of "Save All").
static bool DynamicSaveAll() {
    // Clean tombstoned/orphan files BEFORE writing, so deleted blocks release their storage;
    // positions in the registry are never touched.
    CleanupDynamicFiles();
    for (uint16_t i = 0; i < dynamic_block_registry.block_count && i < MAX_DYNAMIC_BLOCKS; i++) {
        if (!dynamic_block_registry.blocks[i].present) continue;
        if (!SaveDynamicBlockFiles(dynamic_block_registry.blocks[i], i)) return false;
    }
    return true;
}

// Rebuilds every slot from its DT/DV files. Per the docs each block keeps its own files, so
// a slot with no DT file stays a tombstone.
static bool DynamicRecallAll() {
    bool ok = true;
    for (uint16_t i = 0; i < MAX_DYNAMIC_BLOCKS; i++) {
        DynamicBlockDescriptor scratch;
        if (!LoadDynamicBlockFiles(scratch, i)) continue;
        while (dynamic_block_registry.block_count <= i)
            if (!dynamic_block_registry.AddTombstone()) { ok = false; break; }
        if (!ok) break;
        dynamic_block_registry.TombstoneBlock(i);
        *dynamic_block_registry.GetBlock(i) = scratch;
    }
    return ok;
}

static void HandleCreateDynamic(const PacketFrame &frame, uint16_t index) {
    if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
    uint16_t name_len = PayloadBytes(frame) - 4;
    if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;
    DynamicBlockDescriptor *block = CreateDynamicBlock(frame.payload + 4, name_len, index);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + 1];
    BlockIndex out_index = {(uint8_t)index, 0xFF, 0xFF};
    memcpy(payload, &out_index, sizeof(BlockIndex));
    payload[sizeof(BlockIndex)] = 1;
    SendResponse(frame, payload, sizeof(payload));
}

static void HandleDeleteDynamic(const PacketFrame &frame, uint16_t block_idx, uint8_t field_idx, uint8_t key) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    if (field_idx == INVALID_INDEX) { // delete the whole block -> tombstone (slot kept)
        dynamic_block_registry.TombstoneBlock(block_idx);
        RespondStatus(frame, true);
        return;
    }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    if (key != INVALID_INDEX) { RespondStatus(frame, block->DeleteEntry(field_idx, key)); return; }
    // Delete every entry at `field` (the record compacts per removal).
    uint8_t keys[256];
    uint16_t n = block->ListKeys(field_idx, keys, 256);
    bool ok = true;
    for (uint16_t i = 0; i < n; i++)
        ok &= block->DeleteEntry(field_idx, keys[i]);
    RespondStatus(frame, ok);
}

static void HandleGetName(const PacketFrame &frame, uint32_t bi, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + BLOCK_NAME_LEN];
    memcpy(payload, &bi, 4);
    uint16_t n = strlen(block->Name); if (n > BLOCK_NAME_LEN - 1) n = BLOCK_NAME_LEN - 1;
    memcpy(payload + 4, block->Name, n);
    SendResponse(frame, payload, 4 + n);
}

static void HandleSetName(const PacketFrame &frame, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
    uint16_t n = PayloadBytes(frame) - 4; if (n > BLOCK_NAME_LEN - 1) n = BLOCK_NAME_LEN - 1;
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    memcpy(block->Name, frame.payload + 4, n);
    block->Name[n] = '\0';
    RespondStatus(frame, true);
}

#endif

// CID 3 (Recall All) / CID 4 (Save All): the whole device in one command
// (Docs/Services/Register.md - no BlockInfo). Partial saving/recall is the app's job (direct
// file writes / direct register writes), so nothing is addressed here. Each target keeps its
// own bounded read-modify-write - a DAS's whole persistent set does not fit one
// MEMORY_BACKUP_CAP buffer - and the results are aggregated into a single reply.
static void HandleSaveRecallAll(const PacketFrame &frame, bool save) {
    bool ok = true;
    if (save) {
        ok &= StaticSaveAll();
#ifdef USE_DYNAMIC_BLOCKS
        ok &= DynamicSaveAll();
#endif
    } else {
        // A recall has no failure mode of its own: a field with no stored entry is simply left
        // as it is (the doc's reply is Success). Only rebuilding a dynamic slot can fail.
        StaticRecallAll();
#ifdef USE_DYNAMIC_BLOCKS
        ok = DynamicRecallAll();
#endif
    }
    RespondStatus(frame, ok);
}
