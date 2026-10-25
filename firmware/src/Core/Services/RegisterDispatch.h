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
inline bool RegisterService::RegisterResolveByBlockInfo(uint32_t Bi, FieldResult &Fr) {
    Fr = FieldResult{};
    uint16_t Type = BlockInfoType(Bi);
    uint8_t Inst = BlockInfoInstance(Bi);
    uint8_t Field = BlockInfoField(Bi);
    uint8_t Key = BlockInfoKey(Bi);
#ifdef USE_SCRIPTS
    if (BlockTypeRange::IsScript(Type)) { // Script I/O (inputs/outputs)
        ValueInfo Sm;
        void *Ptr = nullptr;
        if (!Script.GetIoPointer(BlockTypeRange::ScriptGlobal(Type, Inst), Field, Key, Sm, Ptr)) return false;
        Fr.Descriptor = Sm;
        Fr.Data = Ptr;
        return true;
    }
#endif
#ifdef USE_DYNAMIC_BLOCKS
    if (BlockTypeRange::IsDynamic(Type)) {
        uint16_t Gi = BlockTypeRange::DynamicGlobal(Type, Inst);
        if (Gi >= DynamicBlockRegistry.BlockCount) return false;
        DynamicBlockDescriptor *Block = DynamicBlockRegistry.GetBlock(Gi);
        if (!Block) return false;
        KeyResult Kr = Block->GetKey(Field, Key);
        if (!Kr.Exists) return false;
        Fr.Descriptor = Kr.Meta;
        Fr.Data = Kr.DataPtr;
        return true;
    }
#endif
    const StaticBlockDescriptor *Blk = FindBlock(Type, Inst);
    if (!Blk) return false;
    FieldResult Result = Blk->Get(Field, Key);
    if (!Result.Data) return false;
    Fr = Result;
    return true;
}

// Generic register access by BlockInfo (shared by the Subscriptions and Script services).
inline bool RegisterService::RegisterGetByBlockInfo(uint32_t Bi, ValueInfo &Meta, uint8_t *Vbuf, uint8_t &Vsz) {
    FieldResult Fr;
    Vsz = 0;
    if (!RegisterResolveByBlockInfo(Bi, Fr)) return false;
    Meta = Fr.Descriptor;
    // A row with no bytes is not readable, however well it resolved (ValueReadable, MemoryTypes.h):
    // an empty dynamic entry resolves to a descriptor with Size 0 and no data. Handing it back as a
    // 0-length value of the row's own type moves the failure into the reader's assignment - a Number
    // too short to decode there - and a script read of this row would surface as SCRIPT_ERR_TYPE,
    // sending the reader after a type problem in the script when the row is what is empty. Failing
    // the access instead carries SCRIPT_ERR_REGISTER (ScriptExec.h's read instruction).
    if (!ValueReadable(Meta)) return false;
    memcpy(Vbuf, Fr.Data, Meta.Size);
    Vsz = Meta.Size;
    return true;
}

// Writes a register value by BlockInfo. `m` carries the value type and the passive flags.
inline bool RegisterService::RegisterSetByBlockInfo(uint32_t Bi, const ValueInfo &Meta, const uint8_t *Val, uint16_t Vlen) {
    uint16_t Type = BlockInfoType(Bi);
    uint8_t Inst = BlockInfoInstance(Bi);
    uint8_t Field = BlockInfoField(Bi);
    uint8_t Key = BlockInfoKey(Bi);
#ifdef USE_SCRIPTS
    if (BlockTypeRange::IsScript(Type)) { // Script I/O: only inputs are writable
        return Script.SetEntry(BlockTypeRange::ScriptGlobal(Type, Inst), Field, Key, Meta, Val, Vlen);
    }
#endif
    if (BlockTypeRange::IsDynamic(Type)) {
#ifdef USE_DYNAMIC_BLOCKS
        uint16_t Gi = BlockTypeRange::DynamicGlobal(Type, Inst);
        if (Gi >= DynamicBlockRegistry.BlockCount) return false;
        DynamicBlockDescriptor *Block = DynamicBlockRegistry.GetBlock(Gi);
        if (!Block) return false;
        return Block->SetEntry(Field, Key, Val, Vlen, Meta);
#else
        return false;
#endif
    }
    const StaticBlockDescriptor *Blk = FindBlock(Type, Inst);
    if (!Blk) return false;
    return Blk->Set(Field, Key, Val, Vlen, Meta);
}

inline bool RegisterService::Handle(const PacketFrame &Frame) {
    uint8_t Cid = GetServiceCID(Frame.Cmd);
    if (Frame.Flags & FLAG_TYPE) return true;

    // Recall All / Save All - the whole device, no BlockInfo (Docs/Services/Register.md).
    // Partial saving/recall is the app's job (direct file writes / direct register writes).
    if (Cid == (uint8_t)RegisterCid::RecallAll) { HandleSaveRecallAll(Frame, false); return true; }
    if (Cid == (uint8_t)RegisterCid::SaveAll)    { HandleSaveRecallAll(Frame, true);  return true; }

    // Enumerate: CID 0 lists the block types, CID 1 a block's Field&Keys. Handled before the
    // BlockInfo guard below (which every other command needs).
    if (Cid == (uint8_t)RegisterCid::EnumerateBlocks) { HandleEnumerateBlocks(Frame); return true; }
    if (Cid == (uint8_t)RegisterCid::EnumerateFields) { HandleEnumerateFields(Frame); return true; }

    // Dynamic management (Docs/Services/Register.md "Dynamic commands"): the request
    // carries the global Index (uint16), not a BlockInfo, so handle it before the
    // BlockInfo guard below.
#ifdef USE_DYNAMIC_BLOCKS
    if (Cid >= (uint8_t)DynamicCid::Create && Cid <= (uint8_t)DynamicCid::SetName) {
        if (PayloadBytes(Frame) < 2) { RespondStatus(Frame, false); return true; }
        uint16_t Gi = LoadUnaligned<uint16_t>(Frame.Payload);
        switch ((DynamicCid)Cid) {
            case DynamicCid::Create:  HandleCreateDynamic(Frame, Gi); return true;
            case DynamicCid::Delete:  HandleDeleteDynamic(Frame, Gi); return true;
            case DynamicCid::GetName: HandleGetName(Frame, Gi); return true;
            case DynamicCid::SetName: HandleSetName(Frame, Gi); return true;
            default: break;
        }
    }
#endif

    if (PayloadBytes(Frame) < 4) { RespondStatus(Frame, false); return true; }

    // frame.payload is 4-byte aligned (PacketFrame is packed+aligned(4)), so this is one
    // word load rather than a 4-byte memcpy.
    uint32_t Bi = *reinterpret_cast<const uint32_t *>(Frame.Payload);
    uint16_t Type = BlockInfoType(Bi);
    uint8_t Inst = BlockInfoInstance(Bi);
    uint8_t Field = BlockInfoField(Bi);
    uint8_t Key = BlockInfoKey(Bi);

    // Read (single entry: the request carries one BlockInfo).
    if (Cid == (uint8_t)RegisterCid::Read) {
        if (PayloadBytes(Frame) != 4) { RespondStatus(Frame,false); return true; }
#ifdef USE_SCRIPTS
        if (BlockTypeRange::IsScript(Type)) {
            HandleScriptBlockRead(Frame, Bi, BlockTypeRange::ScriptGlobal(Type, Inst), Field, Key);
            return true;
        }
#endif
#ifdef USE_DYNAMIC_BLOCKS
        if (BlockTypeRange::IsDynamic(Type)) {
            HandleDynamicBlockRead(Frame, Bi, BlockTypeRange::DynamicGlobal(Type, Inst), Field, Key);
            return true;
        }
#else
        if (BlockTypeRange::IsDynamic(Type)) { RespondStatus(Frame, false); return true; }
#endif
        HandleStaticBlockRead(Frame, Bi, Type, Inst, Field, Key);
        return true;
    }

    // Write.
    if (Cid == (uint8_t)RegisterCid::Write) {
        if (PayloadBytes(Frame) < 8) { RespondStatus(Frame,false); return true; }
        const ValueInfo *Desc = (const ValueInfo *)(Frame.Payload + 4);
        const uint8_t *Val = Frame.Payload+8;
        // The ValueInfo.Size must match the value bytes actually present: a larger Size
        // would make the write path copy past the frame. It is also capped to what one
        // packet can carry, symmetric with the read reply's cap.
        uint16_t Vlen = Desc->Size;
        uint16_t Avail = (uint16_t)(PayloadBytes(Frame) - 8);
        if (Vlen > Avail) Vlen = Avail;
        if (Vlen > FIELD_RESPONSE_MAX_VALUE) Vlen = FIELD_RESPONSE_MAX_VALUE;
#ifdef USE_SCRIPTS
        if (BlockTypeRange::IsScript(Type)) {
            HandleScriptBlockWrite(Frame, BlockTypeRange::ScriptGlobal(Type, Inst), Field, Key, Desc, Val, Vlen);
            return true;
        }
#endif
#ifdef USE_DYNAMIC_BLOCKS
        if (BlockTypeRange::IsDynamic(Type)) {
            HandleDynamicBlockWrite(Frame, BlockTypeRange::DynamicGlobal(Type, Inst), Field, Key, Desc, Val, Vlen);
            return true;
        }
#else
        if (BlockTypeRange::IsDynamic(Type)) { RespondStatus(Frame, false); return true; }
#endif
        HandleStaticBlockWrite(Frame, Type, Inst, Field, Key, Desc, Val, Vlen);
        return true;
    }
    return true;
}
