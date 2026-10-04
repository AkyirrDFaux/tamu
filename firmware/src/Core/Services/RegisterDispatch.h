#pragma once

// Generic BlockInfo accessors and the main dispatcher.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== Main dispatcher =====

// Core BlockInfo routing, shared by the copy-out accessor below and the subscription service
// (which needs the source pointer, not a copy). On success `fr` carries the descriptor and a
// pointer to the source value (null for a size-0 dynamic entry); the bool reports whether the
// BlockInfo resolved. Ordering: script I/O, dynamic entries, then statics/System.
static bool RegisterResolveByBlockInfo(uint32_t bi, FieldResult &fr) {
    fr = FieldResult{};
    uint16_t type = BlockInfoType(bi);
    uint8_t inst = BlockInfoInstance(bi);
    uint8_t field = BlockInfoField(bi);
    uint8_t key = BlockInfoKey(bi);
#ifdef USE_SCRIPTS
    if (BlockTypeRange::IsScript(type)) { // Script I/O (inputs/outputs)
        ValueInfo sm;
        void *p = nullptr;
        if (!ScriptGetIoPointer(BlockTypeRange::ScriptGlobal(type, inst), field, key, sm, p)) return false;
        fr.Descriptor = sm;
        fr.Data = p;
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
        fr.Descriptor = kr.meta;
        fr.Data = kr.data_ptr;
        return true;
    }
#endif
    const StaticBlockDescriptor *blk = FindBlock(type, inst);
    if (!blk) return false;
    FieldResult g = blk->Get(field, key);
    if (!g.Data) return false;
    fr = g;
    return true;
}

// Generic register access by BlockInfo (shared by the Subscriptions and Script services).
bool RegisterGetByBlockInfo(uint32_t bi, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz) {
    FieldResult fr;
    vsz = 0;
    if (!RegisterResolveByBlockInfo(bi, fr)) return false;
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

    // Dynamic management (Docs/Services/Register.md "Dynamic commands"): the request
    // carries the global Index (uint16), not a BlockInfo, so handle it before the
    // BlockInfo guard below.
#ifdef USE_DYNAMIC_BLOCKS
    if (cid >= (uint8_t)DynamicCid::Create && cid <= (uint8_t)DynamicCid::SetName) {
        if (PayloadBytes(frame) < 2) { RespondStatus(frame, false); return; }
        uint16_t gi = (uint16_t)(frame.payload[0] | (frame.payload[1] << 8));
        switch ((DynamicCid)cid) {
            case DynamicCid::Create:  HandleCreateDynamic(frame, gi); return;
            case DynamicCid::Delete:  HandleDeleteDynamic(frame, gi); return;
            case DynamicCid::GetName: HandleGetName(frame, gi); return;
            case DynamicCid::SetName: HandleSetName(frame, gi); return;
            default: break;
        }
    }
#endif

    if (PayloadBytes(frame) < 4) { RespondStatus(frame, false); return; }

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
        // would make the write path copy past the frame. It is also capped to what one
        // packet can carry, symmetric with the read reply's cap.
        uint16_t vlen = desc->Size;
        uint16_t avail = (uint16_t)(PayloadBytes(frame) - 8);
        if (vlen > avail) vlen = avail;
        if (vlen > FIELD_RESPONSE_MAX_VALUE) vlen = FIELD_RESPONSE_MAX_VALUE;
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
}
