#pragma once

// Generic BlockInfo accessors and the main dispatcher.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== Main dispatcher =====

// Generic register access by BlockInfo (shared by the Subscriptions and Script services).
bool RegisterGetByBlockInfo(uint32_t bi, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz) {
    uint16_t type = BlockInfoType(bi);
    uint8_t inst = BlockInfoInstance(bi);
    uint8_t field = BlockInfoField(bi);
    uint8_t key = BlockInfoKey(bi);
    vsz = 0;
#ifdef USE_SCRIPTS
    if (BlockTypeRange::IsScript(type)) { // Script I/O (inputs/outputs)
        ValueInfo sm;
        void *p = nullptr;
        if (!ScriptGetIoPointer(BlockTypeRange::ScriptGlobal(type, inst), field, key, sm, p)) return false;
        m = sm;
        uint8_t n = sm.Size;
        if (n) memcpy(vbuf, p, n);
        vsz = n;
        return true;
    }
#endif
#ifdef USE_DYNAMIC_BLOCKS
    if (BlockTypeRange::IsDynamic(type)) {
        uint16_t gi = BlockTypeRange::DynamicGlobal(type, inst);
        if (gi >= dynamic_block_registry.block_count) return false;
        DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(gi);
        if (!block) return false;
        KeyResult kr = block->GetKey(field, key);
        if (!kr.exists) return false;
        m = kr.meta;
        uint8_t n = kr.meta.Size;
        if (n) memcpy(vbuf, kr.data_ptr, n);
        vsz = n;
        return true;
    }
#endif
    const StaticBlockDescriptor *blk = FindBlock(type, inst);
    if (!blk) return false;
    FieldResult fr = blk->Get(field, key);
    if (!fr.Data) return false;
    m = fr.Descriptor;
    uint8_t n = fr.Descriptor.Size;
    if (n) memcpy(vbuf, fr.Data, n);
    vsz = n;
    return true;
}

// Writes a register value by BlockInfo. `m` carries the value type and the passive flags.
bool RegisterSetByBlockInfo(uint32_t bi, const ValueInfo &m, const uint8_t *val, uint16_t vlen) {
    uint16_t type = BlockInfoType(bi);
    uint8_t inst = BlockInfoInstance(bi);
    uint8_t field = BlockInfoField(bi);
    uint8_t key = BlockInfoKey(bi);
#ifdef USE_SCRIPTS
    if (BlockTypeRange::IsScript(type)) { // Script I/O: only inputs are writable
        return ScriptSetEntry(BlockTypeRange::ScriptGlobal(type, inst), field, key, m, val, vlen);
    }
#endif
    if (BlockTypeRange::IsDynamic(type)) {
#ifdef USE_DYNAMIC_BLOCKS
        uint16_t gi = BlockTypeRange::DynamicGlobal(type, inst);
        if (gi >= dynamic_block_registry.block_count) return false;
        DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(gi);
        if (!block) return false;
        return block->SetEntry(field, key, val, vlen, m);
#else
        return false;
#endif
    }
    const StaticBlockDescriptor *blk = FindBlock(type, inst);
    if (!blk) return false;
    return blk->Set(field, key, val, vlen, m);
}

static void HandleRegister(const PacketFrame &frame) {
    uint8_t cid = GetServiceCID(frame.cmd);
    if (frame.flags & FLAG_TYPE) return;

    // Recall All / Save All - the whole device, no BlockInfo (Docs/Services/Register.md).
    // Partial saving/recall is the app's job (direct file writes / direct register writes).
    if (cid == (uint8_t)RegisterCid::RecallAll) { HandleSaveRecallAll(frame, false); return; }
    if (cid == (uint8_t)RegisterCid::SaveAll)    { HandleSaveRecallAll(frame, true);  return; }

    // Enumerate: CID 0 lists the block types, CID 1 a block's Field&Keys. Handled before the
    // BlockInfo guard below (which every other command needs).
    if (cid == (uint8_t)RegisterCid::EnumerateBlocks) { HandleEnumerateBlocks(frame); return; }
    if (cid == (uint8_t)RegisterCid::EnumerateFields) { HandleEnumerateFields(frame); return; }

    if (PayloadBytes(frame) < 4) return;
    
    // frame.payload is 4-byte aligned (PacketFrame is packed+aligned(4)), so this is one
    // word load rather than a 4-byte memcpy.
    uint32_t bi = *reinterpret_cast<const uint32_t *>(frame.payload);
    uint16_t type = BlockInfoType(bi);
    uint8_t inst = BlockInfoInstance(bi);
    uint8_t field = BlockInfoField(bi);
    uint8_t key = BlockInfoKey(bi);

    // Read (single entry: the request carries one BlockInfo).
    if (cid == (uint8_t)RegisterCid::Read) {
        if (PayloadBytes(frame) != 4) { RespondStatus(frame,false); return; }
#ifdef USE_SCRIPTS
        if (BlockTypeRange::IsScript(type)) {
            HandleScriptBlockRead(frame, bi, BlockTypeRange::ScriptGlobal(type, inst), field, key);
            return;
        }
#endif
#ifdef USE_DYNAMIC_BLOCKS
        if (BlockTypeRange::IsDynamic(type)) {
            HandleDynamicBlockRead(frame, bi, BlockTypeRange::DynamicGlobal(type, inst), field, key);
            return;
        }
#else
        if (BlockTypeRange::IsDynamic(type)) { RespondStatus(frame, false); return; }
#endif
        HandleStaticBlockRead(frame, bi, type, inst, field, key);
        return;
    }

    // Write.
    if (cid == (uint8_t)RegisterCid::Write) {
        if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
        const ValueInfo *desc = (const ValueInfo *)(frame.payload + 4);
        const uint8_t *val = frame.payload+8;
        // The ValueInfo.Size must match the value bytes actually present: a larger Size
        // would make the write path copy past the frame.
        uint16_t vlen = desc->Size;
        uint16_t avail = (uint16_t)(PayloadBytes(frame) - 8);
        if (vlen > avail) vlen = avail;
#ifdef USE_SCRIPTS
        if (BlockTypeRange::IsScript(type)) {
            HandleScriptBlockWrite(frame, BlockTypeRange::ScriptGlobal(type, inst), field, key, desc, val, vlen);
            return;
        }
#endif
#ifdef USE_DYNAMIC_BLOCKS
        if (BlockTypeRange::IsDynamic(type)) {
            HandleDynamicBlockWrite(frame, BlockTypeRange::DynamicGlobal(type, inst), field, key, desc, val, vlen);
            return;
        }
#else
        if (BlockTypeRange::IsDynamic(type)) { RespondStatus(frame, false); return; }
#endif
        HandleStaticBlockWrite(frame, type, inst, field, key, desc, val, vlen);
        return;
    }

    // Dynamic/keyed management (Docs/Services/Register.md "Dynamic commands").
#ifdef USE_DYNAMIC_BLOCKS
    if (BlockTypeRange::IsDynamic(type)) {
        uint16_t gi = BlockTypeRange::DynamicGlobal(type, inst);
        switch ((DynamicCid)cid) {
            case DynamicCid::Create:  HandleCreateDynamic(frame, gi); return;
            case DynamicCid::Delete:  HandleDeleteDynamic(frame, gi, field, key); return;
            case DynamicCid::GetName: HandleGetName(frame, bi, gi); return;
            case DynamicCid::SetName: HandleSetName(frame, gi); return;
            default: break;
        }
        // The old Get Memory Usage (0x14) / Read Backup (0x15) are dropped by the docs: the app
        // reads the DT_/DV_ files directly.
    }
#endif
}
