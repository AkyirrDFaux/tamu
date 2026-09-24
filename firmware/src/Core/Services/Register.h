#pragma once
#include "Core/Functions/Packet.h"
#include "Core/Functions/Memory.h"

#include "Core/Functions/Device.h"
#include "Core/Functions/SysFunctions.h"
#include "Core/Functions/Storage.h"
#include "Core/Functions/AppInterface.h"
#include "Core/Services/StaticMemory.h"
#include "Blocks/DeviceInfo.h"
#ifdef USE_SCRIPTS
#include "Core/Services/Script.h"
#endif

// Register service per Docs/Services/Register.md
// BlockInfo 32b: Type10 | Instance6 | Field8 | Key8
inline uint16_t BlockInfoType(uint32_t bi) { return (bi >> 22) & 0x3FF; }
inline uint8_t BlockInfoInstance(uint32_t bi) { return (bi >> 16) & 0x3F; }
inline uint8_t BlockInfoField(uint32_t bi) { return (bi >> 8) & 0xFF; }
inline uint8_t BlockInfoKey(uint32_t bi) { return bi & 0xFF; }

// Find static block index from Type+Instance
inline int FindStaticBlock(uint16_t type, uint8_t inst) {
    int count=0;
    for (size_t i=0;i<static_block_num;i++) {
        if ((uint16_t)static_block_registry[i].Schema->Type == type) {
            if (count==inst) return (int)i;
            count++;
        }
    }
    return -1;
}

// ===== Helpers for common response patterns =====
// Buffer size for field responses: 4 (BlockInfo) + 4 (BlockMeta) + max field size + padding.
// Dynamic (keyed) fields hold concatenated dictionary entries and can reach the u8 size
// limit of BlockMeta.Size (255 bytes), so the response buffer must cover 4+4+255+pad.
// SCALAR_ONLY targets (DAS) carry no Vector/Matrix fields; the largest field is a 16-byte string.
#ifdef SCALAR_ONLY
#define FIELD_RESPONSE_BUF_SIZE 32
#else
#define FIELD_RESPONSE_BUF_SIZE 268
#endif

static inline void SendBlockMetaResponse(const PacketFrame &frame, uint32_t bi, uint16_t flags_and_type, uint8_t map_count, const char *name) {
    uint8_t rpl[32]; uint16_t pos=0;
    memcpy(rpl+pos, &bi,4); pos+=4;
    BlockMeta m; m.FlagsAndType = flags_and_type; m.Key=0xFF; m.Size=map_count;
    memcpy(rpl+pos, &m,4); pos+=4;
    uint8_t n = name ? (uint8_t)strlen(name) : 0;
    if (n > BLOCK_NAME_LEN - 1) n = BLOCK_NAME_LEN - 1;
    memcpy(rpl+pos, name, n); pos+=n;
    while (pos % 4) rpl[pos++] = 0; // 4-byte alignment
    SendResponse(frame,rpl,pos);
}

static inline void SendFieldResponse(const PacketFrame &frame, uint32_t bi, const FieldResult &fr) {
    uint8_t rpl[FIELD_RESPONSE_BUF_SIZE]; uint16_t pos=0;
    memcpy(rpl+pos, &bi,4); pos+=4;
    memcpy(rpl+pos, &fr.Descriptor,4); pos+=4;
    memcpy(rpl+pos, fr.Data, fr.Descriptor.Size); pos+=fr.Descriptor.Size;
    while(pos%4) rpl[pos++]=0;
    SendResponse(frame,rpl,pos);
}

// Sends one dynamic entry (BlockInfo echo + BlockMeta + value, 4-aligned).
static inline void SendKeyResponse(const PacketFrame &frame, uint32_t bi, const KeyResult &kr) {
    uint8_t rpl[FIELD_RESPONSE_BUF_SIZE]; uint16_t pos = 0;
    memcpy(rpl + pos, &bi, 4); pos += 4;
    memcpy(rpl + pos, &kr.meta, 4); pos += 4;
    if (kr.data_ptr && kr.data_len) memcpy(rpl + pos, kr.data_ptr, kr.data_len);
    pos += kr.data_len;
    while (pos % 4) rpl[pos++] = 0;
    SendResponse(frame, rpl, pos);
}

// ===== CID 0: Enumerate =====

static void HandleEnumerate(const PacketFrame &frame, uint32_t bi) {
    if (PayloadBytes(frame) < 5) { RespondStatus(frame,false); return; }
    uint8_t enum_level = frame.payload[0];
    uint32_t bi_req = 0; memcpy(&bi_req, frame.payload+1, 4);
    
    if (enum_level == 0) { // Enumerate block types
        if (bi_req != 0) { RespondStatus(frame,false); return; }
        uint8_t types[16]; uint8_t n=0;
        for (size_t i=0;i<static_block_num && n<16;i++) {
            uint16_t t = (uint16_t)static_block_registry[i].Schema->Type;
            bool seen=false; for(uint8_t j=0;j<n;j++) if(types[j]==(t&0xFF)) seen=true;
            if(!seen) types[n++]=(uint8_t)t;
        }
        uint8_t rpl[20]; memcpy(rpl, &bi, 4); memcpy(rpl+4, types, n);
        SendResponse(frame, rpl, 4+n);
    } else if (enum_level == 1) { // Enumerate instances of type
        if (bi_req == 0) { RespondStatus(frame,false); return; }
        uint16_t req_type = BlockInfoType(bi_req);
        if (req_type == 0x3FE) {
#ifdef USE_SCRIPTS
            // Scripts: list the loaded slots (stable instance = script slot). The reply is
            // a count (u8) followed by the slot ids so sparse loads stay addressable.
            uint8_t ids[MAX_SCRIPTS];
            uint8_t n = ScriptListInstances(ids, MAX_SCRIPTS);
            uint8_t rpl[8 + MAX_SCRIPTS];
            memcpy(rpl, &bi, 4);
            rpl[4] = n;
            memcpy(rpl + 5, ids, n);
            SendResponse(frame, rpl, 5 + n);
#else
            uint8_t rpl[8]; memcpy(rpl, &bi, 4); rpl[4] = 0;
            SendResponse(frame, rpl, 5);
#endif
        } else if (req_type == 0x3FF) { // Dynamic blocks
#ifndef DISABLE_DYNAMIC_MEMORY
            uint8_t cnt = dynamic_block_registry.block_count;
            uint8_t rpl[8]; memcpy(rpl, &bi, 4); rpl[4] = cnt;
            SendResponse(frame, rpl, 5);
#else
            uint8_t rpl[8]; memcpy(rpl, &bi, 4); rpl[4] = 0;
            SendResponse(frame, rpl, 5);
#endif
        } else {
            uint8_t cnt=0; for(size_t i=0;i<static_block_num;i++) if((uint16_t)static_block_registry[i].Schema->Type==req_type) cnt++;
            uint8_t rpl[8]; memcpy(rpl, &bi, 4); rpl[4]=cnt;
            SendResponse(frame, rpl, 5);
        }
    } else if (enum_level == 2) { // Enumerate fields
        if (bi_req == 0) { RespondStatus(frame,false); return; }
        uint16_t req_type = BlockInfoType(bi_req);
        uint8_t req_inst = BlockInfoInstance(bi_req);
        uint16_t cnt;
        if (req_type == 0 && req_inst == 0) {
            // System block is not in the static registry; its field count is board-aware
            // (nodes like the DAS omit the Core-only NetID and App/CLI fields).
            cnt = SYSTEM_FIELD_COUNT;
        } else if (req_type == 0x3FE) {
#ifdef USE_SCRIPTS
            cnt = ScriptActive(req_inst) ? SCRIPT_FIELD_COUNT : 0;
#else
            cnt = 0;
#endif
        } else if (req_type == 0x3FF) {
#ifndef DISABLE_DYNAMIC_MEMORY
            if (req_inst >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
            DynamicBlockDescriptor *dyn = dynamic_block_registry.GetBlock(req_inst);
            // Return the distinct field indexes after the count (dynamic blocks).
            uint8_t fields[256];
            cnt = dyn->ListFields(fields, 256);
            uint8_t rpl[8 + 256]; memcpy(rpl, &bi_req, 4); rpl[4]=(uint8_t)cnt; rpl[5]=(uint8_t)(cnt>>8);
            uint16_t p = 6;
            for (uint16_t i = 0; i < cnt; i++) rpl[p++] = fields[i];
            SendResponse(frame, rpl, p);
            return;
#else
            cnt = 0;
#endif
        } else {
            int idx = FindStaticBlock(req_type, req_inst);
            if (idx < 0) { RespondStatus(frame,false); return; }
            cnt = static_block_registry[idx].Schema->MapCount;
        }
        uint8_t rpl[8]; memcpy(rpl, &bi_req, 4); rpl[4]=(uint8_t)cnt; rpl[5]=(uint8_t)(cnt>>8);
        SendResponse(frame, rpl, 6);
    } else if (enum_level == 3) { // Enumerate keys in a field (dynamic blocks)
        uint8_t rpl[8 + 256];
        memcpy(rpl, &bi_req, 4);
        uint16_t p = 4;
        uint16_t req_type = BlockInfoType(bi_req);
        if (req_type == 0x3FF) {
#ifndef DISABLE_DYNAMIC_MEMORY
            uint8_t req_inst = BlockInfoInstance(bi_req);
            uint8_t req_field = BlockInfoField(bi_req);
            if (req_inst < dynamic_block_registry.block_count) {
                DynamicBlockDescriptor *dyn = dynamic_block_registry.GetBlock(req_inst);
                uint8_t keys[256];
                uint16_t key_count = dyn->ListKeys(req_field, keys, 256);
                rpl[p++] = (uint8_t)key_count;
                for (uint16_t i = 0; i < key_count; i++) rpl[p++] = keys[i];
            } else {
                rpl[p++] = 0;
            }
#else
            rpl[p++] = 0;
#endif
        } else if (req_type == 0x3FE) {
#ifdef USE_SCRIPTS
            uint8_t n = ScriptKeyCount(BlockInfoInstance(bi_req), BlockInfoField(bi_req));
            rpl[p++] = n;
            for (uint8_t i = 0; i < n; i++) rpl[p++] = i;
#else
            rpl[p++] = 0;
#endif
        } else {
            rpl[p++] = 0;
        }
        SendResponse(frame, rpl, p);
    } else {
        RespondStatus(frame,false);
    }
}

// ===== CID 1: Read helpers =====

// Small helpers to write a system-block field value + descriptor in one place. The
// Subscriptions service resolves system-block sources through the same path.
static inline void SysFieldValue(BlockMeta &m, uint8_t *vbuf, uint8_t &vsz, const void *v, uint8_t size, uint16_t type) {
    memcpy(vbuf, v, size);
    m.FlagsAndType = type; m.Size = size; vsz = size;
}
static inline void SysFieldU32(BlockMeta &m, uint8_t *vbuf, uint8_t &vsz, uint32_t v, uint16_t type) {
    SysFieldValue(m, vbuf, vsz, &v, 4, type);
}

// Resolves one System-block field (type 0, inst 0) into a descriptor + value. Returns
// false for an unknown field/key.
bool RegisterGetSystemField(uint8_t field, uint8_t key, BlockMeta &m, uint8_t *vbuf, uint8_t &vsz) {
    m = {}; vsz = 0;
    if (field==0 && key==0) SysFieldU32(m, vbuf, vsz, (uint32_t)kDeviceType, (uint16_t)DataType::Enum|FieldFlags::ReadOnly);
    else if (field==0 && key==1) SysFieldU32(m, vbuf, vsz, kCapabilities, (uint16_t)DataType::Index|FieldFlags::ReadOnly);
    else if (field==0 && key==2) { uint8_t ver[4] = { (VERSION_YEAR % 100), VERSION_MONTH, VERSION_DAY, VERSION_ITERATION }; SysFieldValue(m, vbuf, vsz, ver, 4, (uint16_t)DataType::String|FieldFlags::ReadOnly); }
    else if (field==1) SysFieldValue(m, vbuf, vsz, GetSerialNumber().bytes, 14, (uint16_t)DataType::SN|FieldFlags::ReadOnly);
    else if (field==2) { uint16_t v=DeviceStatus.ShortAddress; SysFieldValue(m, vbuf, vsz, &v, 2, (uint16_t)DataType::Id|FieldFlags::ReadOnly); }
    else if (field==3 && key==0) SysFieldU32(m, vbuf, vsz, TimeFromBoot(), (uint16_t)DataType::Index|FieldFlags::ReadOnly);
    else if (field==3 && key==1) SysFieldU32(m, vbuf, vsz, Now(), (uint16_t)DataType::Index|FieldFlags::ReadOnly);
    else if (field==3 && key==2) SysFieldU32(m, vbuf, vsz, (uint32_t)CurrentTimeOffsetMs(), (uint16_t)DataType::Index|FieldFlags::ReadOnly);
    else if (field==3 && key==3) SysFieldValue(m, vbuf, vsz, &DeviceStatus.AvgLoopTimeMs, 4, (uint16_t)DataType::Number|FieldFlags::ReadOnly);
    else if (field==3 && key==4) SysFieldValue(m, vbuf, vsz, &DeviceStatus.MaxLoopTimeMs, 4, (uint16_t)DataType::Number|FieldFlags::ReadOnly);
    else if (field==4 && key==0) SysFieldU32(m, vbuf, vsz, (uint32_t)GetFreeRAM(), (uint16_t)DataType::Index|FieldFlags::ReadOnly);
    else if (field==4 && key==1) SysFieldU32(m, vbuf, vsz, GetTotalRAM(), (uint16_t)DataType::Index|FieldFlags::ReadOnly);
    else if (field==5 && key==0) SysFieldU32(m, vbuf, vsz, Storage.UsedFlashBytes(), (uint16_t)DataType::Index|FieldFlags::ReadOnly);
    else if (field==5 && key==1) SysFieldU32(m, vbuf, vsz, STORAGE_FLASH_SIZE, (uint16_t)DataType::Index|FieldFlags::ReadOnly);
    else if (field==6) { m.FlagsAndType=(uint16_t)DataType::String|FieldFlags::Persistent; m.Size=strlen(DeviceName); if(m.Size>16) m.Size=16; vsz=m.Size; memcpy(vbuf, DeviceName, vsz); }
#ifdef TYPE_CORE
    else if (field==7) { uint8_t v=DeviceStatus.NetId; SysFieldValue(m, vbuf, vsz, &v, 1, (uint16_t)DataType::Id|FieldFlags::Persistent); }
#endif
#ifndef BOARD_DAS_v0_1
    else if (field==8 && key==0) { uint8_t v=AppConnected?1:0; SysFieldValue(m, vbuf, vsz, &v, 1, (uint16_t)DataType::Bool|FieldFlags::ReadOnly); }
    else if (field==8 && key==1) { uint8_t v=AppCLIConnected()?1:0; SysFieldValue(m, vbuf, vsz, &v, 1, (uint16_t)DataType::Bool|FieldFlags::ReadOnly); }
#endif
    else { return false; }
    m.Key = key;
    return true;
}

static void HandleSystemBlockRead(const PacketFrame &frame, uint32_t bi, uint8_t field, uint8_t key) {
    uint8_t rpl[40]; uint16_t pos=0;
    memcpy(rpl+pos, &bi,4); pos+=4;
    BlockMeta m = {}; uint8_t vsz=0; uint8_t vbuf[24]={0};
    
    if (field==0xFF) { m.FlagsAndType = (uint16_t)BlockType::System | FieldFlags::ReadOnly; m.Key=0xFF; m.Size=SYSTEM_FIELD_COUNT; memcpy(rpl+pos,&m,4); pos+=4; rpl[pos++]=SYSTEM_FIELD_COUNT; while(pos%4) rpl[pos++]=0; SendResponse(frame,rpl,pos); return; }

    // All system fields resolve through the shared RegisterGetSystemField (single
    // source of truth - the Subscriptions service uses the same path).
    if (!RegisterGetSystemField(field, key, m, vbuf, vsz))
    {
        RespondStatus(frame,false);
        return;
    }
    
    memcpy(rpl+pos, &m, 4); pos += 4;
    memcpy(rpl+pos, vbuf, vsz); pos += vsz;
    while(pos%4) rpl[pos++]=0;
    SendResponse(frame,rpl,pos);
}

static void HandleMultiEntryRead(const PacketFrame &frame, uint32_t bi, uint16_t payload_bytes) {
    uint16_t num_entries = payload_bytes / 4;
    if (num_entries <= 1) return;
    
    for (uint16_t i = 0; i < num_entries; i++) {
        uint32_t bi_entry = 0;
        memcpy(&bi_entry, frame.payload + i * 4, 4);
        int idx = FindStaticBlock(BlockInfoType(bi_entry), BlockInfoInstance(bi_entry));
        if (idx < 0) { RespondStatus(frame,false); return; }
        const StaticBlockDescriptor &blk = static_block_registry[idx];
        if (BlockInfoField(bi_entry) >= blk.Schema->MapCount) { RespondStatus(frame,false); return; }
        FieldResult fr = blk.Get(BlockInfoField(bi_entry));
        if (!fr.Data) { RespondStatus(frame,false); return; }
        SendFieldResponse(frame, bi, fr);
    }
}

#ifndef DISABLE_DYNAMIC_MEMORY
static void HandleDynamicBlockRead(const PacketFrame &frame, uint32_t bi, uint8_t inst, uint8_t field, uint8_t key) {
    if (inst >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
    if (!block) { RespondStatus(frame,false); return; }
    if (field == 0xFF) { // block meta
        SendBlockMetaResponse(frame, bi, (uint16_t)block->type, (uint8_t)block->FieldCount(), block->Name);
        return;
    }
    KeyResult kr = block->GetKey(field, key);
    if (!kr.exists) { RespondStatus(frame,false); return; }
    SendKeyResponse(frame, bi, kr);
}
#endif

#ifdef USE_SCRIPTS
// Reads one entry of a loaded script (block type 0x3FE) or its block meta. The script's
// input/output/variable/constant tables are addressed as fields 1-4 with key = index.
static void HandleScriptBlockRead(const PacketFrame &frame, uint32_t bi, uint8_t inst, uint8_t field, uint8_t key) {
    LoadedScript *s = ScriptActive(inst);
    if (!s) { RespondStatus(frame,false); return; }
    if (field == 0xFF) {
        SendBlockMetaResponse(frame, bi, 0x3FE, SCRIPT_FIELD_COUNT, s->name);
        return;
    }
    uint8_t rpl[FIELD_RESPONSE_BUF_SIZE];
    uint16_t pos = 0;
    BlockMeta m = {};
    uint8_t vbuf[FIELD_RESPONSE_BUF_SIZE];
    uint8_t vsz = 0;
    if (!ScriptGetEntry(inst, field, key, m, vbuf, vsz)) { RespondStatus(frame,false); return; }
    memcpy(rpl + pos, &bi, 4); pos += 4;
    memcpy(rpl + pos, &m, 4); pos += 4;
    if (vsz) memcpy(rpl + pos, vbuf, vsz);
    pos += vsz;
    while (pos % 4) rpl[pos++] = 0;
    SendResponse(frame, rpl, pos);
}
#endif

static void HandleStaticBlockRead(const PacketFrame &frame, uint32_t bi, uint16_t type, uint8_t inst, uint8_t field) {
    int idx = FindStaticBlock(type, inst);
    if (idx < 0) { RespondStatus(frame,false); return; }
    const StaticBlockDescriptor &blk = static_block_registry[idx];
    if (field==0xFF) { // block meta
        SendBlockMetaResponse(frame, bi, (uint16_t)blk.Schema->Type, blk.Schema->MapCount, blk.Name);
        return;
    }
    if (field >= blk.Schema->MapCount) { RespondStatus(frame,false); return; }
    FieldResult fr = blk.Get(field);
    if(!fr.Data) { RespondStatus(frame,false); return; }
    SendFieldResponse(frame, bi, fr);
}

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
        BlockMeta *desc=(BlockMeta*)(frame.payload+4);
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
        BlockMeta *desc=(BlockMeta*)(frame.payload+4);
        const uint8_t *val=frame.payload+8;
        // Docs: 0 is not allowed (it is re-randomised at boot); 0x3F is "all nets".
        if (desc->Size < 1 || val[0] == 0 || val[0] >= 0x3F) { RespondStatus(frame,false); return; }
        uint16_t type = (uint16_t)DataType::Id | FieldFlags::Persistent;
        if (SystemFieldPersistExplicit(SYSTEM_FIELD_NETID, val, 1, type))
            SendResponse(frame,frame.payload,PayloadBytes(frame));
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

// ===== CID 3,4: Save/Recall for Static =====

// Writes (or updates in place) one field entry in the STATLOG mirror buffer. Entry format:
// BlockIndex[4] + BlockMeta[4] + value[4-aligned]. Returns the new buffer length (0 = no
// room / invalid). Reusing the exact same entry for a repeated save keeps the log bounded.
static uint16_t LogEntryWrite(uint8_t block_idx, uint8_t field, uint8_t *buf, uint16_t cnt,
                              const BlockMeta &m, const uint8_t *val, uint8_t vsz) {
    uint16_t c = 0;
    for (; c + kLogEntryHeaderSize <= cnt; ) {
        uint8_t b = buf[c]; if (b == 0xFF) break;
        uint8_t sz = buf[c + kLogEntryHeaderSize - 1];
        uint16_t el = LogEntrySize(sz);
        if (c + el > cnt) break;
        if (buf[c] == block_idx && buf[c + 1] == field) break; // replace the existing entry
        c += el;
    }
    uint16_t need = kLogEntryHeaderSize + ((vsz + 3) & ~3);
    if (c + need > MEMORY_BACKUP_CAP) return 0;
    BlockIndex ei = {block_idx, field, 0xFF, 0};
    memcpy(buf + c, &ei, sizeof(BlockIndex)); c += sizeof(BlockIndex);
    memcpy(buf + c, &m, sizeof(BlockMeta));  c += sizeof(BlockMeta);
    memcpy(buf + c, val, vsz);               c += vsz;
    while (c % 4) buf[c++] = 0;
    return c;
}

// Persists one static-block field into the STATLOG buffer.
static uint16_t StaticFieldSave(uint8_t idx, uint8_t field, uint8_t *buf, uint16_t cnt) {
    const StaticBlockDescriptor &blk = static_block_registry[idx];
    FieldResult fr = blk.Get(field);
    if (!fr.Data) return 0;
    return LogEntryWrite(idx, field, buf, cnt, fr.Descriptor, (const uint8_t *)fr.Data, fr.Descriptor.Size);
}

// Recalls one static-block field from the STATLOG buffer into its RAM backing.
static bool StaticFieldRecall(uint8_t idx, uint8_t field, const uint8_t *buf, uint16_t cnt) {
    for (uint16_t c = 0; c + kLogEntryHeaderSize <= cnt; ) {
        uint8_t b = buf[c]; if (b == 0xFF) break;
        uint8_t sz = buf[c + kLogEntryHeaderSize - 1];
        uint16_t el = LogEntrySize(sz);
        if (c + el > cnt) break;
        if (buf[c] == idx && buf[c + 1] == field) {
            FieldResult fr = static_block_registry[idx].Get(field);
            if (fr.Data && sz == fr.Descriptor.Size) { memcpy(fr.Data, buf + c + kLogEntryHeaderSize, sz); return true; }
            return false;
        }
        c += el;
    }
    return false;
}

// Persists one System-block persistent field (Name 6 / NetID 7) into the STATLOG buffer.
static uint16_t SystemFieldSave(uint8_t field, uint8_t *buf, uint16_t cnt) {
    uint8_t val[16]; uint8_t vsz = 0; uint16_t type = 0;
    if (field == SYSTEM_FIELD_NAME) {
        vsz = (uint8_t)strlen(DeviceName);
        if (vsz > 16) vsz = 16; // docs: Name is a 16-byte field
        memcpy(val, DeviceName, vsz);
        type = (uint16_t)DataType::String | FieldFlags::Persistent;
#ifdef TYPE_CORE
    } else if (field == SYSTEM_FIELD_NETID) {
        val[0] = DeviceStatus.NetId; vsz = 1;
        type = (uint16_t)DataType::Id | FieldFlags::Persistent;
#endif
    } else {
        return 0;
    }
    BlockMeta m; m.FlagsAndType = type; m.Key = 0xFF; m.Size = vsz;
    return LogEntryWrite(SYSTEM_BLOCK_BACKUP, field, buf, cnt, m, val, vsz);
}

// Recalls a System-block persistent field (Name 6 / NetID 7) from STATLOG into RAM.
static bool SystemFieldRecall(uint8_t field, const uint8_t *buf, uint16_t cnt) {
    for (uint16_t c = 0; c + kLogEntryHeaderSize <= cnt; ) {
        uint8_t b = buf[c]; if (b == 0xFF) break;
        uint8_t sz = buf[c + kLogEntryHeaderSize - 1];
        uint16_t el = LogEntrySize(sz);
        if (c + el > cnt) break;
        if (buf[c] == SYSTEM_BLOCK_BACKUP && buf[c + 1] == field) {
            uint16_t val_c = c + kLogEntryHeaderSize;
            if (field == SYSTEM_FIELD_NAME) {
                uint16_t nl = sz; if (nl > 16) nl = 16;
                memcpy(DeviceNameBuffer, buf + val_c, nl);
                DeviceNameBuffer[nl] = '\0';
#ifdef TYPE_CORE
            } else if (field == SYSTEM_FIELD_NETID) {
                DeviceStatus.NetId = buf[val_c];
#endif
            }
            return true;
        }
        c += el;
    }
    return false;
}

static void HandleStaticSaveRecall(const PacketFrame &frame, uint8_t cid, uint32_t bi_save, uint8_t field) {
    uint16_t type = BlockInfoType(bi_save);
    uint8_t inst = BlockInfoInstance(bi_save);

    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t cnt = ReadBackupFile(StaticLogName(), buf, sizeof(buf));

    // System block (type 0, inst 0): its persistent fields (Name 6, NetID 7) live in the
    // SAME static backup file (STATLOG) as the static blocks.
    if (type == 0 && inst == 0) {
        if (cid == 3) { // Save
            uint16_t len = 0;
            bool any = false;
            if (field == SYSTEM_FIELD_NAME || field == 0xFF) {
                uint16_t l = SystemFieldSave(SYSTEM_FIELD_NAME, buf, len ? len : cnt);
                if (l == 0) { RespondStatus(frame,false); return; }
                len = l; any = true;
            }
#ifdef TYPE_CORE
            if (field == SYSTEM_FIELD_NETID || field == 0xFF) {
                uint16_t l = SystemFieldSave(SYSTEM_FIELD_NETID, buf, len ? len : cnt);
                if (l == 0) { RespondStatus(frame,false); return; }
                len = l; any = true;
            }
#endif
            if (!any) { RespondStatus(frame,false); return; }
            RespondStatus(frame, WriteBackupFile(StaticLogName(), buf, len));
        } else { // Recall
            bool any = false;
            if (field == SYSTEM_FIELD_NAME || field == 0xFF)
                any |= SystemFieldRecall(SYSTEM_FIELD_NAME, buf, cnt);
#ifdef TYPE_CORE
            if (field == SYSTEM_FIELD_NETID || field == 0xFF)
                any |= SystemFieldRecall(SYSTEM_FIELD_NETID, buf, cnt);
#endif
            RespondStatus(frame, any);
        }
        return;
    }

    int idx = FindStaticBlock(type, inst);
    if (idx < 0) { RespondStatus(frame,false); return; }
    const StaticBlockDescriptor &blk = static_block_registry[idx];

    if (cid == 3) { // Save
        if (field == 0xFF) { // whole block: persist every writable persistent field
            uint16_t len = 0;
            for (uint16_t fi = 0; fi < blk.Schema->MapCount; fi++) {
                if (!(blk.Schema->Map[fi].FlagsAndType & FieldFlags::Persistent) ||
                    (blk.Schema->Map[fi].FlagsAndType & FieldFlags::ReadOnly)) continue;
                uint16_t l = StaticFieldSave((uint8_t)idx, (uint8_t)fi, buf, len ? len : cnt);
                if (l == 0) { RespondStatus(frame,false); return; }
                len = l;
            }
            if (len == 0) { RespondStatus(frame,true); return; } // nothing writable/persistent
            RespondStatus(frame, WriteBackupFile(StaticLogName(), buf, len));
        } else {
            uint16_t len = StaticFieldSave((uint8_t)idx, field, buf, cnt);
            if (len == 0) { RespondStatus(frame,false); return; }
            RespondStatus(frame, WriteBackupFile(StaticLogName(), buf, len));
        }
    } else { // Recall
        bool ok;
        if (field == 0xFF) {
            ok = true;
            for (uint16_t fi = 0; fi < blk.Schema->MapCount; fi++) {
                if (!(blk.Schema->Map[fi].FlagsAndType & FieldFlags::Persistent) ||
                    (blk.Schema->Map[fi].FlagsAndType & FieldFlags::ReadOnly)) continue;
                if (StaticFieldRecall((uint8_t)idx, (uint8_t)fi, buf, cnt)) ok = true;
                else ok = false;
            }
        } else {
            ok = StaticFieldRecall((uint8_t)idx, field, buf, cnt);
        }
        RespondStatus(frame, ok);
    }
}

#ifndef DISABLE_DYNAMIC_MEMORY
static void HandleDynamicSaveRecall(const PacketFrame &frame, uint8_t cid, uint8_t inst_save, uint8_t field) {
    if (cid == 3) { // Save (DT/DV per-block files)
        // Clean tombstoned/orphan files BEFORE writing, so deleted blocks release
        // their storage; positions in the registry are never touched.
        CleanupDynamicFiles();
        if (inst_save == 0x3F) {
            for (uint16_t i = 0; i < dynamic_block_registry.block_count && i < MAX_DYNAMIC_BLOCKS; i++) {
                if (dynamic_block_registry.blocks[i].type == BlockType::None) continue;
                if (!SaveDynamicBlockFiles(dynamic_block_registry.blocks[i], i)) { RespondStatus(frame,false); return; }
            }
        } else {
            if (inst_save >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
            DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst_save);
            if (!block || block->type == BlockType::None) { RespondStatus(frame,false); return; }
            if (!SaveDynamicBlockFiles(*block, inst_save)) { RespondStatus(frame,false); return; }
        }
        RespondStatus(frame,true);
        return;
    }

    // Recall (CID 4): rebuild the slot(s) from their DT/DV files. Per the docs each
    // block keeps its own files, so a slot with no DT file stays a tombstone.
    if (inst_save == 0x3F) {
        bool ok = true;
        for (uint16_t i = 0; i < MAX_DYNAMIC_BLOCKS; i++) {
            DynamicBlockDescriptor scratch;
            if (!LoadDynamicBlockFiles(scratch, i))
                continue;
            while (dynamic_block_registry.block_count <= i)
                if (!dynamic_block_registry.AddBlock(BlockType::Undefined)) { ok = false; break; }
            if (!ok) break;
            dynamic_block_registry.TombstoneBlock(i);
            *dynamic_block_registry.GetBlock(i) = scratch;
        }
        RespondStatus(frame, ok);
        return;
    }

    DynamicBlockDescriptor scratch;
    if (!LoadDynamicBlockFiles(scratch, inst_save)) { RespondStatus(frame,false); return; }
    while (dynamic_block_registry.block_count <= inst_save)
        if (!dynamic_block_registry.AddBlock(BlockType::Undefined)) { RespondStatus(frame,false); return; }
    dynamic_block_registry.TombstoneBlock(inst_save);
    *dynamic_block_registry.GetBlock(inst_save) = scratch;
    RespondStatus(frame,true);
}

static void HandleCreateDynamic(const PacketFrame &frame, uint8_t index, uint16_t type) {
    if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
    uint16_t name_len = PayloadBytes(frame) - 4;
    if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;
    DynamicBlockDescriptor *block = CreateDynamicBlock((BlockType)type, frame.payload + 4, name_len, index);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + 1];
    BlockIndex out_index = {index, 0xFF, 0xFF};
    memcpy(payload, &out_index, sizeof(BlockIndex));
    payload[sizeof(BlockIndex)] = 1;
    SendResponse(frame, payload, sizeof(payload));
}

static void HandleDeleteDynamic(const PacketFrame &frame, uint16_t block_idx, uint8_t field_idx, uint8_t key) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    if (field_idx == INVALID_INDEX) { // delete the whole block -> tombstone (slot kept)
        dynamic_block_registry.TombstoneBlock(block_idx);
        RespondStatus(frame, true);
        return;
    }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    if (key != INVALID_INDEX) { RespondStatus(frame, block->DeleteEntry(field_idx, key)); return; }
    // Delete every entry at `field` (the record compacts per removal).
    uint8_t keys[256];
    uint16_t n = block->ListKeys(field_idx, keys, 256);
    bool ok = true;
    for (uint16_t i = 0; i < n; i++)
        ok &= block->DeleteEntry(field_idx, keys[i]);
    RespondStatus(frame, ok);
}

static void HandleGetName(const PacketFrame &frame, uint32_t bi, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + BLOCK_NAME_LEN];
    memcpy(payload, &bi, 4);
    uint16_t n = strlen(block->Name); if (n > BLOCK_NAME_LEN - 1) n = BLOCK_NAME_LEN - 1;
    memcpy(payload + 4, block->Name, n);
    SendResponse(frame, payload, 4 + n);
}

static void HandleSetName(const PacketFrame &frame, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
    uint16_t n = PayloadBytes(frame) - 4; if (n > BLOCK_NAME_LEN - 1) n = BLOCK_NAME_LEN - 1;
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    memcpy(block->Name, frame.payload + 4, n);
    block->Name[n] = '\0';
    RespondStatus(frame, true);
}

static void HandleGetMemUsage(const PacketFrame &frame, uint32_t bi, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + 24];
    memcpy(payload, &bi, 4);
    uint32_t *u32 = reinterpret_cast<uint32_t *>(payload + 4);
    u32[0] = block->entry_count * sizeof(DynamicEntry);
    u32[1] = block->entry_allocated * sizeof(DynamicEntry);
    u32[2] = block->volatile_len;
    u32[3] = block->volatile_allocated;
    u32[4] = block->persistent_len;
    u32[5] = block->persistent_allocated;
    SendResponse(frame, payload, sizeof(payload));
}

static void HandleReadBackup(const PacketFrame &frame, uint32_t bi, uint16_t block_idx) {
    if (block_idx >= MAX_DYNAMIC_BLOCKS) { RespondStatus(frame,false); return; }
    uint8_t req_field = BlockInfoField(bi);
    uint8_t req_key = BlockInfoKey(bi);

    DynamicBlockDescriptor scratch;
    if (!LoadDynamicBlockFiles(scratch, block_idx)) { RespondStatus(frame,false); return; }

    if (req_field == 0xFF) {
        SendBlockMetaResponse(frame, bi, (uint16_t)scratch.type, (uint8_t)scratch.FieldCount(), scratch.Name);
    } else {
        KeyResult kr = scratch.GetKey(req_field, req_key);
        if (!kr.exists) RespondStatus(frame, false);
        else SendKeyResponse(frame, bi, kr);
    }
    scratch.Release();
}

#endif

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

// Writes a register value by BlockInfo. `m` carries the value type (and any active flags).
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
    return static_block_registry[idx].Set(field, val, vlen, m.FlagsAndType);
}

static void HandleRegister(const PacketFrame &frame) {
    uint8_t cid = GetServiceCID(frame.cmd);
    if (frame.flags & FLAG_TYPE) return;
    if (PayloadBytes(frame) < 4) return;
    
    uint32_t bi = 0; memcpy(&bi, frame.payload, 4);
    uint16_t type = BlockInfoType(bi);
    uint8_t inst = BlockInfoInstance(bi);
    uint8_t field = BlockInfoField(bi);
    uint8_t key = BlockInfoKey(bi);

    // CID 0: Enumerate
    if (cid == 0) { HandleEnumerate(frame, bi); return; }

    // CID 1: Read
    if (cid == 1) {
        if (type==0 && inst==0) { HandleSystemBlockRead(frame, bi, field, key); return; }
        HandleMultiEntryRead(frame, bi, PayloadBytes(frame));
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
        BlockMeta *desc = (BlockMeta*)(frame.payload+4);
        const uint8_t *val = frame.payload+8;
        uint16_t vlen = desc->Size;
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

    // CID 3,4: Save/Recall
    if (cid == 3 || cid == 4) {
        if (PayloadBytes(frame) < 4) { RespondStatus(frame,false); return; }
        uint32_t bi_save = 0; memcpy(&bi_save, frame.payload, 4);
        uint16_t type_save = BlockInfoType(bi_save);
        uint8_t inst_save = BlockInfoInstance(bi_save);

        if (type_save == 0x3FF) {
#ifndef DISABLE_DYNAMIC_MEMORY
            HandleDynamicSaveRecall(frame, cid, inst_save, field);
#else
            RespondStatus(frame, false);
#endif
        } else if (type_save == 0 || FindStaticBlock(type_save, inst_save) >= 0) {
            // System block (type 0) + static blocks persist through the STATLOG mirror.
            HandleStaticSaveRecall(frame, cid, bi_save, field);
        } else {
            RespondStatus(frame,false);
        }
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