#pragma once
#include "Core/Functions/Packet.h"
#include "Core/Functions/Memory.h"

#include "Core/Functions/Device.h"
#include "Core/Functions/SysFunctions.h"
#include "Core/Functions/Storage.h"
#include "Core/Functions/AppInterface.h"
#include "Core/Services/StaticMemory.h"
#include "Blocks/DeviceInfo.h"

// Register service per Docs/Services/Register.md
// BlockInfo 32b: Type10 | Instance6 | Field8 | Key8
inline uint32_t MakeBlockInfo(uint16_t type, uint8_t inst, uint8_t field, uint8_t key) {
    return ((uint32_t)(type & 0x3FF) << 22) | ((uint32_t)(inst & 0x3F) << 16) | ((uint32_t)field << 8) | key;
}
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
// Buffer size for field responses: 4 (BlockInfo) + 4 (BlockMeta) + max field size (Matrix<3,3> = 40) + padding = 56
#define FIELD_RESPONSE_BUF_SIZE 64

static inline void SendBlockMetaResponse(const PacketFrame &frame, uint32_t bi, uint16_t flags_and_type, uint8_t map_count, const char *name) {
    uint8_t rpl[32]; uint16_t pos=0;
    memcpy(rpl+pos, &bi,4); pos+=4;
    BlockMeta m; m.FlagsAndType = flags_and_type; m.Key=0xFF; m.Size=map_count;
    memcpy(rpl+pos, &m,4); pos+=4;
    uint8_t n = name ? strlen(name) : 0; if (n > 8) n = 8;
    memcpy(rpl+pos, name, n); pos+=4;
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
        if (req_type == 0x3FF) { // Dynamic blocks
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
        int idx=FindStaticBlock(req_type, req_inst);
        if(idx<0) { RespondStatus(frame,false); return; }
        uint16_t cnt=static_block_registry[idx].Schema->MapCount;
        uint8_t rpl[8]; memcpy(rpl, &bi_req, 4); rpl[4]=(uint8_t)cnt; rpl[5]=(uint8_t)(cnt>>8);
        SendResponse(frame, rpl, 6);
    } else if (enum_level == 3) { // Enumerate keys (static blocks not keyed)
        uint8_t rpl[8]; memcpy(rpl, &bi, 4); rpl[4]=0;
        SendResponse(frame, rpl, 5);
    } else {
        RespondStatus(frame,false);
    }
}

// ===== CID 1: Read helpers =====

static void HandleSystemBlockRead(const PacketFrame &frame, uint32_t bi, uint8_t field, uint8_t key) {
    uint8_t rpl[40]; uint16_t pos=0;
    memcpy(rpl+pos, &bi,4); pos+=4;
    BlockMeta m = {}; uint8_t vsz=0; uint8_t vbuf[24]={0};
    
    if (field==0xFF) { m.FlagsAndType = (uint16_t)BlockType::System | FieldFlags::ReadOnly; m.Key=0xFF; m.Size=9; memcpy(rpl+pos,&m,4); pos+=4; rpl[pos++]=9; while(pos%4) rpl[pos++]=0; SendResponse(frame,rpl,pos); return; }
    if (field==0 && key==0) { uint32_t v=(uint32_t)kDeviceType; memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Enum|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==0 && key==1) { uint32_t v=kCapabilities; memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Index|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==0 && key==2) { uint8_t ver[4] = { (VERSION_YEAR % 100), VERSION_MONTH, VERSION_DAY, VERSION_ITERATION }; m.FlagsAndType=(uint16_t)DataType::String|FieldFlags::ReadOnly; m.Size=4; vsz=4; memcpy(vbuf, ver, 4); }
    else if (field==1) { m.FlagsAndType=(uint16_t)DataType::SN|FieldFlags::ReadOnly; m.Size=14; vsz=14; memcpy(vbuf, GetSerialNumber().bytes, 14); }
    else if (field==2) { uint16_t v=DeviceStatus.ShortAddress; memcpy(vbuf,&v,2); m.FlagsAndType=(uint16_t)DataType::Id|FieldFlags::ReadOnly; m.Size=2; vsz=2; }
    else if (field==3 && key==0) { uint32_t v=TimeFromBoot(); memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Index|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==3 && key==1) { uint32_t v=Now(); memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Index|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==3 && key==2) { int32_t v=TimeOffsetMs; memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Index|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==3 && key==3) { Number v=DeviceStatus.AvgLoopTimeMs; memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Number|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==3 && key==4) { Number v=DeviceStatus.MaxLoopTimeMs; memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Number|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==4 && key==0) { uint32_t v=GetFreeRAM(); memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Index|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==4 && key==1) { uint32_t v=GetTotalRAM(); memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Index|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==5 && key==0) { uint32_t v=Storage.UsedFlashBytes(); memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Index|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==5 && key==1) { uint32_t v=STORAGE_FLASH_SIZE; memcpy(vbuf,&v,4); m.FlagsAndType=(uint16_t)DataType::Index|FieldFlags::ReadOnly; m.Size=4; vsz=4; }
    else if (field==6) { LoadPersistedDeviceName(); m.FlagsAndType=(uint16_t)DataType::String|FieldFlags::Persistent; m.Size=strlen(DeviceName); if(m.Size>16) m.Size=16; vsz=m.Size; memcpy(vbuf, DeviceName, vsz); }
    else if (field==7) { uint16_t v=DeviceStatus.ShortAddress; memcpy(vbuf,&v,2); m.FlagsAndType=(uint16_t)DataType::Id|FieldFlags::Persistent; m.Size=2; vsz=2; }
    else if (field==8 && key==0) { uint8_t v=AppConnected?1:0; vbuf[0]=v; m.FlagsAndType=(uint16_t)DataType::Bool|FieldFlags::ReadOnly; m.Size=1; vsz=1; }
    else if (field==8 && key==1) { uint8_t v=AppCLIConnected()?1:0; vbuf[0]=v; m.FlagsAndType=(uint16_t)DataType::Bool|FieldFlags::ReadOnly; m.Size=1; vsz=1; }
    else { RespondStatus(frame,false); return; }
    
    m.Key=key;
    memcpy(rpl+pos+4, vbuf, vsz);
    memcpy(rpl+pos,&m,4);
    pos+=4+vsz;
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
static void HandleDynamicBlockRead(const PacketFrame &frame, uint32_t bi, uint8_t inst, uint8_t field) {
    if (inst >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
    if (!block) { RespondStatus(frame,false); return; }
    if (field == 0xFF) { // block meta
        SendBlockMetaResponse(frame, bi, (uint16_t)block->type, block->map_count, block->Name);
        return;
    }
    if (field >= block->map_count) { RespondStatus(frame,false); return; }
    FieldResult fr = block->Get(field);
    if(!fr.Data) { RespondStatus(frame,false); return; }
    SendFieldResponse(frame, bi, fr);
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

static void HandleSystemBlockWrite(const PacketFrame &frame, uint8_t field) {
    if (field==6) { // Name
        if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
        BlockMeta *desc=(BlockMeta*)(frame.payload+4);
        const uint8_t *val=frame.payload+8;
        uint16_t len=desc->Size; if(len>24) len=24; memcpy(DeviceNameBuffer,val,len); DeviceNameBuffer[len]='\0'; PersistDeviceName(); SendResponse(frame,frame.payload,PayloadBytes(frame));
    } else {
        RespondStatus(frame,false);
    }
}

#ifndef DISABLE_DYNAMIC_MEMORY
static void HandleDynamicBlockWrite(const PacketFrame &frame, uint8_t inst, uint8_t field, BlockMeta *desc, const uint8_t *val, uint16_t vlen) {
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
        BlockIndex out_index = {(uint8_t)(dynamic_block_registry.block_count - 1), 0xFF, 0xFF};
        memcpy(payload, &out_index, sizeof(BlockIndex));
        payload[sizeof(BlockIndex)] = 1;
        SendResponse(frame, payload, sizeof(payload));
        return;
    }
    
    if (field < block->map_count) {
        if (!block->Set(field, val, vlen, desc->FlagsAndType)) { RespondStatus(frame,false); return; }
    } else if (field == block->map_count) {
        BlockMeta meta = *desc; meta.Size = (uint8_t)vlen;
        if (!block->InsertField(field, meta) || !block->Set(field, val, vlen, desc->FlagsAndType)) { RespondStatus(frame,false); return; }
    } else {
        while (block->map_count < field) {
            BlockMeta pad = {}; pad.FlagsAndType = (uint16_t)DataType::None;
            if (!block->InsertField(block->map_count, pad)) { RespondStatus(frame,false); return; }
        }
        if (block->map_count != field) { RespondStatus(frame,false); return; }
        BlockMeta meta = *desc; meta.Size = (uint8_t)vlen;
        if (!block->InsertField(field, meta) || !block->Set(field, val, vlen, desc->FlagsAndType)) { RespondStatus(frame,false); return; }
    }
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

static void HandleStaticSaveRecall(const PacketFrame &frame, uint8_t cid, uint32_t bi_save, uint8_t field) {
    uint16_t type = BlockInfoType(bi_save);
    uint8_t inst = BlockInfoInstance(bi_save);
    int idx = FindStaticBlock(type, inst);
    if (idx < 0) { RespondStatus(frame,false); return; }
    const StaticBlockDescriptor &blk = static_block_registry[idx];
    FieldResult fr = blk.Get(field);
    if (!fr.Data) { RespondStatus(frame,false); return; }
    
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t cnt = ReadBackupFile(StaticLogName(), buf, sizeof(buf));
    
    if (cid == 3) { // Save - direct memory mirror
        uint16_t write_pos = 0;
        uint16_t c = 0;
        for (; c + 8 <= cnt; ) {
            uint8_t b = buf[c];
            if (b == 0xFF) break;
            uint8_t sz = buf[c + 5];
            uint16_t el = 8 + ((sz + 3) & ~3);
            if (c + el > cnt) break;
            if (buf[c] == 0 && buf[c + 1] == field) { break; }
            c += el;
        }
        write_pos = c;
        uint16_t need = 8 + ((fr.Descriptor.Size + 3) & ~3);
        if (write_pos + need > sizeof(buf)) { RespondStatus(frame,false); return; }
        BlockIndex ei = {(uint8_t)idx, field, 0xFF, 0};
        memcpy(buf + write_pos, &ei, 4); write_pos += 4;
        memcpy(buf + write_pos, &fr.Descriptor, 4); write_pos += 4;
        memcpy(buf + write_pos, fr.Data, fr.Descriptor.Size); write_pos += fr.Descriptor.Size;
        while (write_pos % 4) buf[write_pos++] = 0;
        if (!WriteBackupFile(StaticLogName(), buf, write_pos)) { RespondStatus(frame,false); return; }
        RespondStatus(frame,true);
    } else { // Recall
        bool found = false;
        for (uint16_t c = 0; c + 8 <= cnt; ) {
            uint8_t b = buf[c]; if (b == 0xFF) break;
            uint8_t sz = buf[c + 5]; uint16_t el = 8 + ((sz + 3) & ~3);
            if (c + el > cnt) break;
            if (buf[c] == 0 && buf[c + 1] == field) {
                uint16_t val_c = c + 8;
                FieldResult fr2 = static_block_registry[idx].Get(field);
                if (fr2.Data && sz == fr2.Descriptor.Size) { memcpy(fr2.Data, buf + val_c, sz); found = true; }
                break;
            }
            c += el;
        }
        RespondStatus(frame, found);
    }
}

#ifndef DISABLE_DYNAMIC_MEMORY
static void HandleDynamicSaveRecall(const PacketFrame &frame, uint8_t cid, uint8_t inst_save, uint8_t field) {
    bool save_all = (inst_save == 0x3F);
    DynamicBlockDescriptor *block = nullptr;
    if (!save_all) {
        if (inst_save >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
        block = dynamic_block_registry.GetBlock(inst_save);
        if (!block) { RespondStatus(frame,false); return; }
    }
    
    if (cid == 3) { // Save
        uint8_t backup_buf[MEMORY_BACKUP_CAP];
        uint16_t serialized_len = SerializeRegistry(dynamic_block_registry, backup_buf, sizeof(backup_buf));
        if (serialized_len == 0) { RespondStatus(frame,false); return; }
        if (!WriteBackupFile(DynamicBackupName(), backup_buf, serialized_len)) { RespondStatus(frame,false); return; }
        RespondStatus(frame,true);
    } else { // Recall
        if (save_all) { RespondStatus(frame,false); }
        else if (RecallRegistryBlock(dynamic_block_registry, inst_save, DynamicBackupName())) { RespondStatus(frame,true); }
        else { RespondStatus(frame,false); }
    }
}

static void HandleCreateDynamic(const PacketFrame &frame, uint16_t type) {
    if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
    uint16_t name_len = PayloadBytes(frame) - 4;
    if (name_len > 11) name_len = 11;
    DynamicBlockDescriptor *block = CreateDynamicBlock((BlockType)type, frame.payload + 4, name_len);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + 1];
    BlockIndex out_index = {(uint8_t)(dynamic_block_registry.block_count - 1), 0xFF, 0xFF};
    memcpy(payload, &out_index, sizeof(BlockIndex));
    payload[sizeof(BlockIndex)] = 1;
    SendResponse(frame, payload, sizeof(payload));
}

static void HandleDeleteDynamic(const PacketFrame &frame, uint16_t block_idx, uint8_t field_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    if (field_idx == INVALID_INDEX) { dynamic_block_registry.RemoveBlock(block_idx); RespondStatus(frame, true); }
    else if (field_idx < block->map_count) { RespondStatus(frame, block->Remove(field_idx)); }
    else { RespondStatus(frame, false); }
}

static void HandleGetName(const PacketFrame &frame, uint32_t bi, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + 12];
    memcpy(payload, &bi, 4);
    uint16_t n = strlen(block->Name); if (n > 12) n = 12;
    memcpy(payload + 4, block->Name, n);
    SendResponse(frame, payload, 4 + n);
}

static void HandleSetName(const PacketFrame &frame, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
    uint16_t n = PayloadBytes(frame) - 4; if (n > 11) n = 11;
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
    u32[0] = block->map_count * sizeof(BlockMeta);
    u32[1] = block->map_allocated * sizeof(BlockMeta);
    u32[2] = block->length;
    u32[3] = block->allocated;
    u32[4] = block->length;
    u32[5] = block->allocated;
    SendResponse(frame, payload, sizeof(payload));
}

static void HandleReadBackup(const PacketFrame &frame, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    
    // Parse BlockInfo from payload
    uint32_t bi = 0; memcpy(&bi, frame.payload, 4);
    uint8_t req_inst = BlockInfoInstance(bi);
    uint8_t req_field = BlockInfoField(bi);
    uint8_t req_key = BlockInfoKey(bi);
    
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t cnt = ReadBackupFile(DynamicBackupName(), buf, sizeof(buf));
    if (cnt == 0) { RespondStatus(frame,false); return; }
    
    uint8_t payload[MAX_PAYLOAD_SIZE];
    bool found = false;
    if (cnt >= 2) {
        uint16_t cursor = 2;
        uint16_t block_index = 0;
        while (cursor < cnt) {
            if (cursor + 1 > cnt) break;
            uint8_t name_length = buf[cursor++];
            if (cursor + name_length > cnt) break;
            cursor += name_length;
            if (cursor + 2 > cnt) break;
            uint16_t type; memcpy(&type, buf + cursor, 2); cursor += 2;
            if (cursor + 2 > cnt) break;
            uint16_t map_count; memcpy(&map_count, buf + cursor, 2); cursor += 2;
            uint16_t map_bytes = map_count * sizeof(BlockMeta);
            if (cursor + map_bytes > cnt) break;
            const BlockMeta *map = (const BlockMeta *)(buf + cursor); cursor += map_bytes;
            if (cursor + 2 > cnt) break;
            uint16_t data_length; memcpy(&data_length, buf + cursor, 2); cursor += 2;
            if (cursor + data_length > cnt) break;
            const uint8_t *data = buf + cursor; cursor += data_length;

            if (block_index == req_inst) {
                const BlockMeta &fd = map[req_field];
                if (req_key == INVALID_INDEX) { // Dictionary read
                    if (fd.Size > 0) {
                        uint16_t base = 0;
                        for (uint16_t i = 0; i < req_field; i++) base += AlignTo4(map[i].Size);
                        if (base + fd.Size <= data_length) {
                            const uint8_t *dict_data = data + base;
                            uint16_t key_count = 0, key_offset = 0;
                            while (key_offset + sizeof(BlockMeta) <= fd.Size) {
                                const BlockMeta *m = (const BlockMeta *)(dict_data + key_offset);
                                if (!KeyedEntryFits(m->Size, key_offset, fd.Size)) break;
                                if (((uint16_t)m->FlagsAndType & 0x03FF) != (uint16_t)DataType::None) key_count++;
                                key_offset += AlignTo4(sizeof(BlockMeta) + m->Size);
                            }
                            if (sizeof(BlockIndex) + sizeof(BlockMeta) + key_count <= MAX_PAYLOAD_SIZE) {
                                BlockIndex out_idx = {req_inst, req_field, INVALID_INDEX};
                                memcpy(payload, &out_idx, sizeof(BlockIndex));
                                BlockMeta dm = fd; dm.Size = (uint8_t)key_count;
                                memcpy(payload + sizeof(BlockIndex), &dm, sizeof(BlockMeta));
                                key_offset = 0;
                                uint16_t p = sizeof(BlockIndex) + sizeof(BlockMeta);
                                while (key_offset + sizeof(BlockMeta) <= fd.Size) {
                                    const BlockMeta *m = (const BlockMeta *)(dict_data + key_offset);
                                    if (!KeyedEntryFits(m->Size, key_offset, fd.Size)) break;
                                    if (((uint16_t)m->FlagsAndType & 0x03FF) != (uint16_t)DataType::None) {
                                        if (p + 1 <= MAX_PAYLOAD_SIZE) payload[p++] = m->Key;
                                    }
                                    key_offset += AlignTo4(sizeof(BlockMeta) + m->Size);
                                }
                                SendResponse(frame, payload, p);
                                found = true;
                            }
                        }
                } else { // Keyed entry read
                    if (fd.Size > 0) {
                        uint16_t base = 0;
                        for (uint16_t i = 0; i < req_field; i++) base += AlignTo4(map[i].Size);
                        if (base + fd.Size <= data_length) {
                            const uint8_t *dict_data = data + base;
                            uint16_t key_offset = 0;
                            while (key_offset + sizeof(BlockMeta) <= fd.Size) {
                                const BlockMeta *m = (const BlockMeta *)(dict_data + key_offset);
                                if (!KeyedEntryFits(m->Size, key_offset, fd.Size)) break;
                                if (m->Key == req_key) {
                                    if (sizeof(BlockIndex) + sizeof(BlockMeta) + m->Size <= MAX_PAYLOAD_SIZE) {
                                        BlockIndex out_idx = {req_inst, req_field, req_key};
                                        memcpy(payload, &out_idx, sizeof(BlockIndex));
                                        memcpy(payload + sizeof(BlockIndex), m, sizeof(BlockMeta));
                                        memcpy(payload + sizeof(BlockIndex) + sizeof(BlockMeta), data + base + key_offset + sizeof(BlockMeta), m->Size);
                                        SendResponse(frame, payload, sizeof(BlockIndex) + sizeof(BlockMeta) + m->Size);
                                        found = true;
                                    }
                                    break;
                                }
                                key_offset += AlignTo4(sizeof(BlockMeta) + m->Size);
                            }
                        }
                    }
                }
                break;
                }
                block_index++;
            }
        }
    }
    if (!found) RespondStatus(frame, false);
}
#endif

// ===== Main dispatcher =====

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
#ifndef DISABLE_DYNAMIC_MEMORY
        if (type == 0x3FF) { HandleDynamicBlockRead(frame, bi, inst, field); return; }
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
#ifndef DISABLE_DYNAMIC_MEMORY
        if (type == 0x3FF) { HandleDynamicBlockWrite(frame, inst, field, desc, val, vlen); return; }
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
        
        if (type_save == 0 && inst_save == 0) { HandleStaticSaveRecall(frame, cid, bi_save, field); }
        else if (type_save == 0x3FF) {
#ifndef DISABLE_DYNAMIC_MEMORY
            HandleDynamicSaveRecall(frame, cid, inst_save, field);
#else
            RespondStatus(frame, false);
#endif
        } else { RespondStatus(frame,false); }
        return;
    }

    // CID 0x10-0x15: Dynamic/Keyed management
#ifndef DISABLE_DYNAMIC_MEMORY
    if (cid >= 0x10 && cid <= 0x15) {
        if (type != 0x3FF) { RespondStatus(frame,false); return; }
        uint16_t block_idx = BlockInfoInstance(bi);
        
        switch (cid) {
            case 0x10: HandleCreateDynamic(frame, type); break;
            case 0x11: HandleDeleteDynamic(frame, block_idx, BlockInfoField(bi)); break;
            case 0x12: HandleGetName(frame, bi, block_idx); break;
            case 0x13: HandleSetName(frame, block_idx); break;
            case 0x14: HandleGetMemUsage(frame, bi, block_idx); break;
            case 0x15: HandleReadBackup(frame, block_idx); break;
        }
    }
#endif
}