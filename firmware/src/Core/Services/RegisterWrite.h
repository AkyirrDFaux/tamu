#pragma once

// CID 3: write helpers.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 3: Write helpers =====

#ifdef USE_DYNAMIC_BLOCKS
static void HandleDynamicBlockWrite(const PacketFrame &frame, uint16_t inst, uint8_t field, uint8_t key, const ValueInfo *desc, const uint8_t *val, uint16_t vlen) {
    if (inst >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
    if (!block || !block->present) { RespondStatus(frame,false); return; }

    if (field == INVALID_INDEX) { // set block name; writing type None deletes the block
        if (ValueInfoType(*desc) == (uint16_t)DataType::None) {
            dynamic_block_registry.TombstoneBlock(inst);
            RespondStatus(frame, true);
            return;
        }
        SetBlockName(block->Name, (const char *)val, vlen);
        SendBlockIndexAck(frame, (uint8_t)inst);
        return;
    }

    // Position-specified entry write; writing type None deletes the entry.
    if (!block->SetEntry(field, key, val, vlen, *desc)) { RespondStatus(frame,false); return; }
    SendResponse(frame, frame.payload, PayloadBytes(frame));
}
#endif

#ifdef USE_SCRIPTS
// Writes a script input (field 1) or variable (field 3) entry.
static void HandleScriptBlockWrite(const PacketFrame &frame, uint16_t inst, uint8_t field, uint8_t key, const ValueInfo *desc, const uint8_t *val, uint16_t vlen) {
    if (!ScriptActive(inst)) { RespondStatus(frame,false); return; }
    if (!ScriptSetEntry(inst, field, key, *desc, val, vlen)) { RespondStatus(frame,false); return; }
    SendResponse(frame, frame.payload, PayloadBytes(frame));
}
#endif

static void HandleStaticBlockWrite(const PacketFrame &frame, uint16_t type, uint8_t inst, uint8_t field, uint8_t key, const ValueInfo *desc, const uint8_t *val, uint16_t vlen) {
    const StaticBlockDescriptor *blk = FindBlock(type, inst);
    if(!blk) { RespondStatus(frame,false); return; }
    if(!blk->Set(field, key, val, vlen, *desc)) { RespondStatus(frame,false); return; }
    SendResponse(frame, frame.payload, PayloadBytes(frame));
}
