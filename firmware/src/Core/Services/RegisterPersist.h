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

    // The reduced filesystem (DAS) has no file-presence bit: its fixed-size `.SV` always reports
    // a full size, so a never-written (or Formatted) file reads back erased (0xFF) rather than
    // "absent". The System Name is the mirror's validity anchor: it is always space-padded text,
    // so a 0xFF byte there means the whole mirror was never validly written. Treat that as "no
    // backup" and persist the compiled-in defaults, so the mirror becomes valid for the app's
    // read-modify-write partial saves instead of clobbering the defaults (and the Name) with
    // erased bytes - the DAS used to come up with a 0xFF name after every reflash.
    for (uint16_t i = 0; i < SYSTEM_NAME_LEN && i < cnt; i++) {
        if (buf[i] == 0xFF) {
            StaticSaveAll();
            return;
        }
    }

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

// Rebuilds slot `i` from its DT/DV files; a slot with no DT file stays a tombstone. Returns
// false when the registry cannot grow to hold the slot.
static bool RestoreDynamicBlock(uint16_t i) {
    DynamicBlockDescriptor scratch;
    if (!LoadDynamicBlockFiles(scratch, i)) return true; // absent file = empty slot
    while (dynamic_block_registry.block_count <= i)
        if (!dynamic_block_registry.AddTombstone()) { scratch.Release(); return false; }
    dynamic_block_registry.TombstoneBlock(i);
    *dynamic_block_registry.GetBlock(i) = scratch;
    return true;
}

// Rebuilds every slot from its DT/DV files. Per the docs each block keeps its own files, so
// a slot with no DT file stays a tombstone.
static bool DynamicRecallAll() {
    for (uint16_t i = 0; i < MAX_DYNAMIC_BLOCKS; i++)
        if (!RestoreDynamicBlock(i)) return false;
    return true;
}

static void HandleCreateDynamic(const PacketFrame &frame, uint16_t index) {
    if (PayloadBytes(frame) < 2) { RespondStatus(frame,false); return; }
    uint16_t name_len = PayloadBytes(frame) - 2;
    DynamicBlockDescriptor *block = CreateDynamicBlock(frame.payload + 2, name_len, index);
    if (!block) { RespondStatus(frame,false); return; }
    SendBlockIndexAck(frame, (uint8_t)index);
}

static void HandleDeleteDynamic(const PacketFrame &frame, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    // Delete Dynamic tombstones the whole block; entry/field deletion is the basic Write
    // with type None (Docs/Services/Register.md "Dynamic commands").
    dynamic_block_registry.TombstoneBlock(block_idx);
    RespondStatus(frame, true);
}

static void HandleGetName(const PacketFrame &frame, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    SendResponse(frame, (const uint8_t *)block->Name, BLOCK_NAME_LEN);
}

static void HandleSetName(const PacketFrame &frame, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    if (PayloadBytes(frame) < 2) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    SetBlockName(block->Name, (const char *)(frame.payload + 2), (uint16_t)(PayloadBytes(frame) - 2));
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
