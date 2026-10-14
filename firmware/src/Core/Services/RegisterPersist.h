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
        // Absent is not the same as unreadable. A short read of a PRESENT .SV is a failed read, and
        // answering it by writing would replace the saved settings with the compiled-in defaults -
        // so only a genuinely absent file is created here. The stored space is left untouched.
        if (Storage.FileExists(StaticValuesName()) != 0xFFFFFFFF) {
            DeviceLog("STORAGE", "static recall: .SV unreadable, left as it is");
            return;
        }
        // No mirror at all: keep the compiled-in defaults, but persist them now so the
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
        // Tombstone only on genuine ABSENCE. A DT file that exists but cannot be read or decoded
        // is a failed read, not a removed block, and a failed read deletes nothing - the slot is
        // left as it stands.
        char Tn[8];
        DynamicTableName(i, Tn);
        if (Storage.FileExists(Tn) != 0xFFFFFFFF) {
            // Keep the files: the slot must also be spared by the next Save All's orphan cleanup,
            // which would otherwise delete a block it could not read.
            ProtectDynamicSlot(i);
            DeviceLog("STORAGE", "recall: slot %d has an unreadable DT, kept",
                      (int)i);
            return true;
        }
        // No DT file: any in-session live block still occupying this slot is stale, so tombstone
        // it instead of leaving it untouched (Recall All is reachable at runtime).
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
//
// The files are enumerated once instead of probing all 256 slots: each probe is a lookup in the
// fixed 16 KB table, which makes a boot take seconds.
//
// Nothing is deleted unless the enumeration completed. A walk that fails part way - or a table
// that cannot be read at all - reports no files, and treating that as "flash is empty" would
// tombstone every block. A failed read deletes nothing: the recall gives up instead.
inline bool RegisterService::DynamicRecallAll() {
    uint16_t Present[MAX_DYNAMIC_BLOCKS];
    uint16_t PresentCount = 0;
    bool Complete = Storage.ForEachFile([&](const char Name[8]) {
        if (Name[2] != 'T') return; // the DT file is the block's existence marker
        uint16_t Idx;
        if (!ParseDynamicFileName(Name, &Idx)) return;
        for (uint16_t k = 0; k < PresentCount; k++)
            if (Present[k] == Idx) return;
        if (PresentCount < MAX_DYNAMIC_BLOCKS)
            Present[PresentCount++] = Idx;
    });
    if (!Complete) {
        DeviceLog("STORAGE", "recall: file table unreadable, leaving blocks alone");
        return false;
    }
    // Only slots with no DT file are tombstoned, so a block whose file exists but will not load is
    // kept rather than dropped.
    for (uint16_t i = 0; i < DynamicBlockRegistry.BlockCount; i++) {
        bool HasFile = false;
        for (uint16_t k = 0; k < PresentCount; k++)
            if (Present[k] == i) { HasFile = true; break; }
        if (!HasFile)
            DynamicBlockRegistry.TombstoneBlock(i);
    }
    for (uint16_t k = 0; k < PresentCount; k++)
        if (!RestoreDynamicBlock(Present[k])) return false;
    DeviceLog("STORAGE", "recall: %d block file(s)", (int)PresentCount);
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
