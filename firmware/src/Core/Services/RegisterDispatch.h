#pragma once

// Generic BlockInfo accessors and the main dispatcher.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== Main dispatcher =====

// Generic register access by BlockInfo (shared by the Subscriptions and Script services).
bool RegisterGetByBlockInfo(uint32_t bi, BlockMeta &m, uint8_t *vbuf, uint8_t &vsz) {
    uint16_t type = BlockInfoType(bi);
    uint8_t inst = BlockInfoInstance(bi);
    uint8_t field = BlockInfoField(bi);
    uint8_t key = BlockInfoKey(bi);
    vsz = 0;
    if (type == 0 && inst == 0)
        return RegisterGetSystemField(field, key, m, vbuf, vsz);
#ifdef USE_SCRIPTS
    if (type == 0x3FE) { // Script I/O (inputs/outputs)
        BlockMeta sm;
        void *p = nullptr;
        if (!ScriptGetIoPointer(inst, field, key, sm, p)) return false;
        m = sm;
        uint8_t n = sm.Size;
        if (n) memcpy(vbuf, p, n);
        vsz = n;
        return true;
    }
#endif
#ifndef DISABLE_DYNAMIC_MEMORY
    if (type == 0x3FF) {
        if (inst >= dynamic_block_registry.block_count) return false;
        DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
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
    int idx = FindStaticBlock(type, inst);
    if (idx < 0) return false;
    FieldResult fr = static_block_registry[idx].Get(field);
    if (!fr.Data) return false;
    m = fr.Descriptor;
    uint8_t n = fr.Descriptor.Size;
    if (n) memcpy(vbuf, fr.Data, n);
    vsz = n;
    return true;
}

// Writes a register value by BlockInfo. `m` carries the value type and the passive flags.
bool RegisterSetByBlockInfo(uint32_t bi, const BlockMeta &m, const uint8_t *val, uint16_t vlen) {
    uint16_t type = BlockInfoType(bi);
    uint8_t inst = BlockInfoInstance(bi);
    uint8_t field = BlockInfoField(bi);
    uint8_t key = BlockInfoKey(bi);
    (void)key; // only the dynamic (keyed) path uses it
#ifdef USE_SCRIPTS
    if (type == 0x3FE) { // Script I/O: only inputs are writable
        return ScriptSetEntry(inst, field, key, m, val, vlen);
    }
#endif
    if (type == 0x3FF) {
#ifndef DISABLE_DYNAMIC_MEMORY
        if (inst >= dynamic_block_registry.block_count) return false;
        DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
        if (!block) return false;
        return block->SetEntry(field, key, val, vlen, m.FlagsAndType);
#else
        return false;
#endif
    }
    int idx = FindStaticBlock(type, inst);
    if (idx < 0) return false;
    if (!static_block_registry[idx].Set(field, val, vlen, m.FlagsAndType)) return false;
    return true;
}

static void HandleRegister(const PacketFrame &frame) {
    uint8_t cid = GetServiceCID(frame.cmd);
    if (frame.flags & FLAG_TYPE) return;

    // CID 3,4: Recall All / Save All - the whole device, no BlockInfo, so an empty request
    // (Docs/Services/Register.md). Handled before the BlockInfo guard below, which every other
    // command needs. Partial saving/recall is the app's job (direct file writes / direct
    // register writes), so nothing is addressed here.
    if (cid == 3 || cid == 4) { HandleSaveRecallAll(frame, cid == 4); return; }

    // CID 0: Enumerate. Its requests are shaped by the command itself - empty for the type list,
    // a packed (type<<6|instance) word for a block's fields+keys - so it is handled before the
    // BlockInfo guard below (which every other command needs).
    if (cid == 0) { HandleEnumerate(frame); return; }

    if (PayloadBytes(frame) < 4) return;
    
    uint32_t bi = 0; memcpy(&bi, frame.payload, 4);
    uint16_t type = BlockInfoType(bi);
    uint8_t inst = BlockInfoInstance(bi);
    uint8_t field = BlockInfoField(bi);
    uint8_t key = BlockInfoKey(bi);

    // CID 1: Read (single entry: the request carries one BlockInfo, per Docs/Services/Register.md)
    if (cid == 1) {
        if (PayloadBytes(frame) != 4) { RespondStatus(frame,false); return; }
        if (type==0 && inst==0) { HandleSystemBlockRead(frame, bi, field, key); return; }
#ifdef USE_SCRIPTS
        if (type == 0x3FE) { HandleScriptBlockRead(frame, bi, inst, field, key); return; }
#endif
#ifndef DISABLE_DYNAMIC_MEMORY
        if (type == 0x3FF) { HandleDynamicBlockRead(frame, bi, inst, field, key); return; }
#else
        if (type == 0x3FF) { RespondStatus(frame, false); return; }
#endif
        HandleStaticBlockRead(frame, bi, type, inst, field);
        return;
    }

    // CID 2: Write
    if (cid == 2) {
        if (type==0 && inst==0) { HandleSystemBlockWrite(frame, field); return; }
        if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
        const ValueInfo *vin = (const ValueInfo *)(frame.payload + 4);
        BlockMeta descMeta = FromWireInfo(*vin);
        BlockMeta *desc = &descMeta;
        const uint8_t *val = frame.payload+8;
        // The BlockMeta.Size must match the value bytes actually present: a larger Size
        // would make the write path copy past the frame (the System Name path clamps too).
        uint16_t vlen = desc->Size;
        uint16_t avail = (uint16_t)(PayloadBytes(frame) - 8);
        if (vlen > avail) vlen = avail;
#ifdef USE_SCRIPTS
        if (type == 0x3FE) { HandleScriptBlockWrite(frame, inst, field, key, desc, val, vlen); return; }
#endif
#ifndef DISABLE_DYNAMIC_MEMORY
        if (type == 0x3FF) { HandleDynamicBlockWrite(frame, inst, field, key, desc, val, vlen); return; }
#else
        if (type == 0x3FF) { RespondStatus(frame, false); return; }
#endif
        HandleStaticBlockWrite(frame, type, inst, field, desc, val, vlen);
        return;
    }

    // CID 0x10-0x15: Dynamic/Keyed management
#ifndef DISABLE_DYNAMIC_MEMORY
    if (cid >= 0x10 && cid <= 0x15) {
        if (type != 0x3FF) { RespondStatus(frame,false); return; }
        uint16_t block_idx = BlockInfoInstance(bi);
        
        switch (cid) {
            case 0x10: HandleCreateDynamic(frame, (uint8_t)block_idx, type); break;
            case 0x11: HandleDeleteDynamic(frame, block_idx, BlockInfoField(bi), BlockInfoKey(bi)); break;
            case 0x12: HandleGetName(frame, bi, block_idx); break;
            case 0x13: HandleSetName(frame, block_idx); break;
            case 0x14: HandleGetMemUsage(frame, bi, block_idx); break;
            case 0x15: HandleReadBackup(frame, bi, block_idx); break;
        }
    }
#endif
}
