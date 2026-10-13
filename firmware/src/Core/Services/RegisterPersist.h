#pragma once

// CID 4/5 save+recall and the 0x10-0x13 dynamic management.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 4/5: Save/Recall for Static =====
//
// The static memory's persistent half is one flat space (the board's `StaticPer`, Docs/Services/
// Register.md "Persistence"), mirrored 1:1 to `.SV`. The space already holds the live System
// fields and every static block's persistent half, so a save is a single copy out and a recall a
// single copy in; targeted saves/recalls are the app's job (a direct file write at the field's
// offset).

static_assert(sizeof(StaticPer) <= MEMORY_BACKUP_CAP,
              "static persistent space must fit the backup buffer");

inline bool RegisterService::StaticSaveAll() {
    // Snapshot the live System fields into the space (the only persistent values not already
    // written through it), then mirror the whole space to .SV in one write.
#ifdef TYPE_CORE
    StaticPer.System.NetId = DeviceStatus.NetId;
#endif
    return WriteBackupFile(StaticValuesName(), (const uint8_t *)&StaticPer, sizeof(StaticPer));
}

inline void RegisterService::StaticRecallAll() {
    uint8_t Buf[sizeof(StaticPer)];
    uint16_t Cnt = ReadBackupFile(StaticValuesName(), Buf, sizeof(Buf));
    if (Cnt < sizeof(StaticPer)) {
        // No (or short) backup: keep the compiled-in defaults, but persist them now so the
        // mirror exists with the right size for the app's read-modify-write partial saves (a
        // direct write to an absent `.SV` cannot succeed).
        StaticSaveAll();
        return;
    }

    // Defensive: a present-but-erased mirror (0xFF) is not a valid saved space. The filesystem
    // erases a file's blocks when it is created, so a valid `.SV` is never all 0xFF; this only
    // guards a torn write. The System Name is the validity anchor (always NUL-padded text since B10),
    // so a 0xFF byte there means the mirror was never validly written - re-persist the live
    // values rather than clobbering the defaults and the Name with erased bytes.
    for (uint16_t i = 0; i < SYSTEM_NAME_LEN && i < Cnt; i++) {
        if (Buf[i] == 0xFF) {
            StaticSaveAll();
            return;
        }
    }

    memcpy(&StaticPer, Buf, sizeof(StaticPer));
#ifdef TYPE_CORE
    DeviceStatus.NetId = StaticPer.System.NetId;
#endif
}

#ifdef USE_DYNAMIC_BLOCKS
// Saves every dynamic block to its DT_/DV_ files (the dynamic half of "Save All").
inline bool RegisterService::DynamicSaveAll() {
    // Clean tombstoned/orphan files BEFORE writing, so deleted blocks release their storage;
    // positions in the registry are never touched.
    CleanupDynamicFiles();
    for (uint16_t i = 0; i < DynamicBlockRegistry.BlockCount && i < MAX_DYNAMIC_BLOCKS; i++) {
        if (!DynamicBlockRegistry.Blocks[i].Present) continue;
        if (!SaveDynamicBlockFiles(DynamicBlockRegistry.Blocks[i], i)) return false;
    }
    return true;
}

// Rebuilds slot `i` from its DT/DV files; a slot with no DT file is tombstoned. Returns
// false when the registry cannot grow to hold the slot.
inline bool RegisterService::RestoreDynamicBlock(uint16_t i) {
    DynamicBlockDescriptor Scratch;
    if (!LoadDynamicBlockFiles(Scratch, i)) {
        // No DT file: any in-session live block still occupying this slot is stale, so
        // tombstone it instead of leaving it untouched (Recall All is reachable at runtime).
        if (i < DynamicBlockRegistry.BlockCount)
            DynamicBlockRegistry.TombstoneBlock(i);
        return true; // absent file = empty slot
    }
    while (DynamicBlockRegistry.BlockCount <= i)
        if (!DynamicBlockRegistry.AddTombstone()) { Scratch.Release(); return false; }
    DynamicBlockRegistry.TombstoneBlock(i);
    *DynamicBlockRegistry.GetBlock(i) = Scratch;
    return true;
}

// Rebuilds every slot from its DT/DV files. Per the docs each block keeps its own files, so
// a slot with no DT file is tombstoned (it might still hold an in-session live block).
inline bool RegisterService::DynamicRecallAll() {
    for (uint16_t i = 0; i < MAX_DYNAMIC_BLOCKS; i++)
        if (!RestoreDynamicBlock(i)) return false;
    return true;
}

inline void RegisterService::HandleCreateDynamic(const PacketFrame &Frame, uint16_t Index) {
    if (PayloadBytes(Frame) < 2) { RespondStatus(Frame,false); return; }
    // The global index is one byte on the wire's dynamic address space, bounded by the
    // registry/cleanup/save loops (all MAX_DYNAMIC_BLOCKS). A larger index would make
    // AddBlockAt pad tombstones past `block_count` so the block is enumerated (8 bits,
    // truncated) but never saved or cleaned up.
    if (Index >= MAX_DYNAMIC_BLOCKS) { RespondStatus(Frame,false); return; }
    uint16_t NameLen = PayloadBytes(Frame) - 2;
    DynamicBlockDescriptor *Block = CreateDynamicBlock(Frame.Payload + 2, NameLen, Index);
    if (!Block) { RespondStatus(Frame,false); return; }
    RespondStatus(Frame, true); // the host knows the index it asked for (Docs: no BlockIndex echo)
}

inline void RegisterService::HandleDeleteDynamic(const PacketFrame &Frame, uint16_t BlockIdx) {
    if (BlockIdx >= DynamicBlockRegistry.BlockCount) { RespondStatus(Frame,false); return; }
    // Delete Dynamic tombstones the whole block; entry/field deletion is the basic Write
    // with type None (Docs/Services/Register.md "Dynamic commands").
    DynamicBlockRegistry.TombstoneBlock(BlockIdx);
    RespondStatus(Frame, true);
}

inline void RegisterService::HandleGetName(const PacketFrame &Frame, uint16_t BlockIdx) {
    if (BlockIdx >= DynamicBlockRegistry.BlockCount) { RespondStatus(Frame,false); return; }
    DynamicBlockDescriptor *Block = DynamicBlockRegistry.GetBlock(BlockIdx);
    if (!Block || !Block->Present) { RespondStatus(Frame,false); return; }
    SendResponse(Frame, (const uint8_t *)Block->Name, BLOCK_NAME_LEN);
}

inline void RegisterService::HandleSetName(const PacketFrame &Frame, uint16_t BlockIdx) {
    if (BlockIdx >= DynamicBlockRegistry.BlockCount) { RespondStatus(Frame,false); return; }
    if (PayloadBytes(Frame) < 2) { RespondStatus(Frame,false); return; }
    DynamicBlockDescriptor *Block = DynamicBlockRegistry.GetBlock(BlockIdx);
    if (!Block || !Block->Present) { RespondStatus(Frame,false); return; }
    SetBlockName(Block->Name, (const char *)(Frame.Payload + 2), (uint16_t)(PayloadBytes(Frame) - 2));
    RespondStatus(Frame, true);
}

#endif

// CID 4 (Recall All) / CID 5 (Save All): the whole device in one command
// (Docs/Services/Register.md - no BlockInfo). Partial saving/recall is the app's job (direct
// file writes / direct register writes), so nothing is addressed here. Each target keeps its
// own bounded read-modify-write - a DAS's whole persistent set does not fit one
// MEMORY_BACKUP_CAP buffer - and the results are aggregated into a single reply.
inline void RegisterService::HandleSaveRecallAll(const PacketFrame &Frame, bool Save) {
    bool Ok = true;
    if (Save) {
        Ok &= StaticSaveAll();
#ifdef USE_DYNAMIC_BLOCKS
        Ok &= DynamicSaveAll();
#endif
    } else {
        // A recall has no failure mode of its own: a field with no stored entry is simply left
        // as it is (the doc's reply is Success). Only rebuilding a dynamic slot can fail.
        StaticRecallAll();
#ifdef USE_DYNAMIC_BLOCKS
        Ok = DynamicRecallAll();
#endif
    }
    RespondStatus(Frame, Ok);
}
