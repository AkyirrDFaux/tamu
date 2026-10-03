#pragma once

// CID 2: write helpers.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 2: Write helpers =====

// Persists one System-block field with an EXPLICIT value (does not read the live value).
// Used by the NetID write: the docs say the NetID applies only after reboot, so the write
// must store it WITHOUT changing the live DeviceStatus.NetId - re-addressing the core at
// runtime would break the bus/app link to it.
static bool SystemFieldPersistExplicit(uint8_t field, const uint8_t *val, uint8_t vsz,
                                       uint16_t type, uint8_t flags) {
    (void)type; (void)flags;
    if (field == SYSTEM_FIELD_NETID && vsz >= 1) {
#ifdef TYPE_CORE
        staticPer.system.NetId = val[0];
#endif
    }
    return WriteBackupFile(StaticValuesName(), (const uint8_t *)&staticPer, sizeof(staticPer));
}

static void HandleSystemBlockWrite(const PacketFrame &frame, uint8_t field) {
    if (field==6) { // Name: fixed 16-char field, space-padded (not a C string)
        if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
        const ValueInfo *desc=(const ValueInfo*)(frame.payload+4);
        const uint8_t *val=frame.payload+8;
        uint16_t len = desc->Size;
        if (len > SYSTEM_NAME_LEN) len = SYSTEM_NAME_LEN;
        if (len > (uint16_t)(PayloadBytes(frame) - 8)) len = (uint16_t)(PayloadBytes(frame) - 8);
        memset(staticPer.system.Name, ' ', SYSTEM_NAME_LEN);
        memcpy(staticPer.system.Name, val, len);
        SendResponse(frame,frame.payload,PayloadBytes(frame));
#ifdef TYPE_CORE
    } else if (field==7) { // NetID (core only): stored now, applied on the next boot
        if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
        const ValueInfo *desc=(const ValueInfo*)(frame.payload+4);
        const uint8_t *val=frame.payload+8;
        // Docs: 0 is not allowed (it is re-randomised at boot); 0x3F is "all nets".
        if (desc->Size < 1 || val[0] == 0 || val[0] >= 0x3F) { RespondStatus(frame,false); return; }
        if (SystemFieldPersistExplicit(SYSTEM_FIELD_NETID, val, 1, (uint16_t)DataType::Id, ValuePersistent))
        {
            SendResponse(frame,frame.payload,PayloadBytes(frame));
        }
        else
            RespondStatus(frame,false);
#endif
    } else {
        RespondStatus(frame,false);
    }
}

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
        uint8_t payload[sizeof(BlockIndex) + 1];
        BlockIndex out_index = {(uint8_t)inst, 0xFF, 0xFF};
        memcpy(payload, &out_index, sizeof(BlockIndex));
        payload[sizeof(BlockIndex)] = 1;
        SendResponse(frame, payload, sizeof(payload));
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
    int idx=FindStaticBlock(type, inst);
    if(idx<0) { RespondStatus(frame,false); return; }
    const StaticBlockDescriptor &blk = static_block_registry[idx];
    if(!blk.Set(field, key, val, vlen, *desc)) { RespondStatus(frame,false); return; }
    SendResponse(frame, frame.payload, PayloadBytes(frame));
}
