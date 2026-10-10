#pragma once

// CID 3: write helpers.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 3: Write helpers =====

#ifdef USE_DYNAMIC_BLOCKS
inline void RegisterService::HandleDynamicBlockWrite(const PacketFrame &Frame, uint16_t Inst, uint8_t Field, uint8_t Key, const ValueInfo *Desc, const uint8_t *Val, uint16_t Vlen) {
    if (Inst >= DynamicBlockRegistry.BlockCount) { RespondStatus(Frame,false); return; }
    DynamicBlockDescriptor *Block = DynamicBlockRegistry.GetBlock(Inst);
    if (!Block || !Block->Present) { RespondStatus(Frame,false); return; }

    if (Field == INVALID_INDEX) { // set block name; writing type None deletes the block
        if (ValueInfoType(*Desc) == (uint16_t)DataType::None) {
            DynamicBlockRegistry.TombstoneBlock(Inst);
            RespondStatus(Frame, true);
            return;
        }
        SetBlockName(Block->Name, (const char *)Val, Vlen);
        SendBlockIndexAck(Frame, (uint8_t)Inst);
        return;
    }

    // Position-specified entry write; writing type None deletes the entry.
    if (!Block->SetEntry(Field, Key, Val, Vlen, *Desc)) { RespondStatus(Frame,false); return; }
    SendResponse(Frame, Frame.Payload, PayloadBytes(Frame));
}
#endif

#ifdef USE_SCRIPTS
// Writes a script input (field 1) or variable (field 3) entry.
inline void RegisterService::HandleScriptBlockWrite(const PacketFrame &Frame, uint16_t Inst, uint8_t Field, uint8_t Key, const ValueInfo *Desc, const uint8_t *Val, uint16_t Vlen) {
    if (!Script.Active(Inst)) { RespondStatus(Frame,false); return; }
    if (!Script.SetEntry(Inst, Field, Key, *Desc, Val, Vlen)) { RespondStatus(Frame,false); return; }
    SendResponse(Frame, Frame.Payload, PayloadBytes(Frame));
}
#endif

inline void RegisterService::HandleStaticBlockWrite(const PacketFrame &Frame, uint16_t Type, uint8_t Inst, uint8_t Field, uint8_t Key, const ValueInfo *Desc, const uint8_t *Val, uint16_t Vlen) {
    const StaticBlockDescriptor *Blk = FindBlock(Type, Inst);
    if(!Blk) { RespondStatus(Frame,false); return; }
    if(!Blk->Set(Field, Key, Val, Vlen, *Desc)) { RespondStatus(Frame,false); return; }
    SendResponse(Frame, Frame.Payload, PayloadBytes(Frame));
}
