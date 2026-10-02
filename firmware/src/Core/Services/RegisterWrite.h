#pragma once

// CID 2: write helpers.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 2: Write helpers =====

// Defined with the Save/Recall helpers below (forward declaration for the write path).
static uint16_t LogEntryWrite(uint8_t block_idx, uint8_t field, uint8_t *buf, uint16_t cnt,
                              const BlockMeta &m, const uint8_t *val, uint8_t vsz);

// Persists one System-block field with an EXPLICIT value (does not read the live value).
// Used by the NetID write: the docs say the NetID applies only after reboot, so the write
// must store it WITHOUT changing the live DeviceStatus.NetId - re-addressing the core at
// runtime would break the bus/app link to it.
static bool SystemFieldPersistExplicit(uint8_t field, const uint8_t *val, uint8_t vsz, uint16_t type) {
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t cnt = ReadBackupFile(StaticLogName(), buf, sizeof(buf));
    BlockMeta m; m.FlagsAndType = type; m.Key = 0xFF; m.Size = vsz;
    uint16_t len = LogEntryWrite(SYSTEM_BLOCK_BACKUP, field, buf, cnt, m, val, vsz);
    if (len == 0) return false;
    return WriteBackupFile(StaticLogName(), buf, len);
}

static void HandleSystemBlockWrite(const PacketFrame &frame, uint8_t field) {
    if (field==6) { // Name
        if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
        const ValueInfo *vin=(const ValueInfo*)(frame.payload+4);
        BlockMeta descMeta = FromWireInfo(*vin);
        BlockMeta *desc = &descMeta;
        const uint8_t *val=frame.payload+8;
        // Docs: Name is a 16-byte field. Clamp to it (and to the received payload) so the
        // terminating NUL can never write past DeviceNameBuffer[24].
        uint16_t len = desc->Size;
        if (len > 16) len = 16;
        if (len > (uint16_t)(PayloadBytes(frame) - 8)) len = (uint16_t)(PayloadBytes(frame) - 8);
        memcpy(DeviceNameBuffer, val, len);
        DeviceNameBuffer[len] = '\0';
        SendResponse(frame,frame.payload,PayloadBytes(frame));
#ifdef TYPE_CORE
    } else if (field==7) { // NetID (core only): stored now, applied on the next boot
        if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
        const ValueInfo *vin=(const ValueInfo*)(frame.payload+4);
        BlockMeta descMeta = FromWireInfo(*vin);
        BlockMeta *desc = &descMeta;
        const uint8_t *val=frame.payload+8;
        // Docs: 0 is not allowed (it is re-randomised at boot); 0x3F is "all nets".
        if (desc->Size < 1 || val[0] == 0 || val[0] >= 0x3F) { RespondStatus(frame,false); return; }
        uint16_t type = (uint16_t)DataType::Id | FieldFlags::Persistent;
        if (SystemFieldPersistExplicit(SYSTEM_FIELD_NETID, val, 1, type))
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

#ifndef DISABLE_DYNAMIC_MEMORY
static void HandleDynamicBlockWrite(const PacketFrame &frame, uint8_t inst, uint8_t field, uint8_t key, BlockMeta *desc, const uint8_t *val, uint16_t vlen) {
    if (inst >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
    if (!block || block->type == BlockType::Deleted) { RespondStatus(frame,false); return; }

    if (field == INVALID_INDEX) { // set block name and/or type
        block->type = (BlockType)BlockMetaType(desc->FlagsAndType);
        uint16_t name_len = vlen;
        if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;
        memcpy(block->Name, val, name_len);
        block->Name[name_len] = '\0';
        uint8_t payload[sizeof(BlockIndex) + 1];
        BlockIndex out_index = {(uint8_t)inst, 0xFF, 0xFF};
        memcpy(payload, &out_index, sizeof(BlockIndex));
        payload[sizeof(BlockIndex)] = 1;
        SendResponse(frame, payload, sizeof(payload));
        return;
    }

    // Position-specified entry write; writing type None deletes the entry.
    if (!block->SetEntry(field, key, val, vlen, desc->FlagsAndType)) { RespondStatus(frame,false); return; }
    SendResponse(frame, frame.payload, PayloadBytes(frame));
}
#endif

#ifdef USE_SCRIPTS
// Writes a script input (field 1) or variable (field 3) entry.
static void HandleScriptBlockWrite(const PacketFrame &frame, uint8_t inst, uint8_t field, uint8_t key, BlockMeta *desc, const uint8_t *val, uint16_t vlen) {
    if (!ScriptActive(inst)) { RespondStatus(frame,false); return; }
    if (!ScriptSetEntry(inst, field, key, *desc, val, vlen)) { RespondStatus(frame,false); return; }
    SendResponse(frame, frame.payload, PayloadBytes(frame));
}
#endif

static void HandleStaticBlockWrite(const PacketFrame &frame, uint16_t type, uint8_t inst, uint8_t field, BlockMeta *desc, const uint8_t *val, uint16_t vlen) {
    int idx=FindStaticBlock(type, inst);
    if(idx<0) { RespondStatus(frame,false); return; }
    const StaticBlockDescriptor &blk = static_block_registry[idx];
    if(!blk.Set(field, val, vlen, desc->FlagsAndType)) { RespondStatus(frame,false); return; }
    SendResponse(frame, frame.payload, PayloadBytes(frame));
}

