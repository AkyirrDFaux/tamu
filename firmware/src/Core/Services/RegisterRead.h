#pragma once

// CID 1: read helpers (System, multi-entry, static, dynamic, script).
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 1: Read helpers =====

// Small helpers to write a system-block field value + descriptor in one place. The
// Subscriptions service resolves system-block sources through the same path.
static inline void SysFieldValue(ValueInfo &m, uint8_t *vbuf, uint8_t &vsz, const void *v, uint8_t size,
                                 uint16_t type, uint8_t flags = 0) {
    memcpy(vbuf, v, size);
    m.Type = type; m.Size = size; m.Flags = flags; vsz = size;
}
static inline void SysFieldU32(ValueInfo &m, uint8_t *vbuf, uint8_t &vsz, uint32_t v, uint16_t type,
                               uint8_t flags = 0) {
    SysFieldValue(m, vbuf, vsz, &v, 4, type, flags);
}

// Resolves one System-block field (type 0, inst 0) into a descriptor + value. Returns
// false for an unknown field/key.
bool RegisterGetSystemField(uint8_t field, uint8_t key, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz) {
    m = {}; vsz = 0;
    if (field==0 && key==0) SysFieldU32(m, vbuf, vsz, (uint32_t)kDeviceType, (uint16_t)DataType::Enum, ValueReadOnly);
    else if (field==0 && key==1) SysFieldU32(m, vbuf, vsz, kCapabilities, (uint16_t)DataType::Index, ValueReadOnly);
    else if (field==0 && key==2) { uint8_t ver[4] = { (VERSION_YEAR % 100), VERSION_MONTH, VERSION_DAY, VERSION_ITERATION }; SysFieldValue(m, vbuf, vsz, ver, 4, (uint16_t)DataType::String, ValueReadOnly); }
    else if (field==1) SysFieldValue(m, vbuf, vsz, GetSerialNumber().bytes, 14, (uint16_t)DataType::SN, ValueReadOnly);
    else if (field==2) { uint16_t v=DeviceStatus.ShortAddress; SysFieldValue(m, vbuf, vsz, &v, 2, (uint16_t)DataType::Id, ValueReadOnly); }
    else if (field==3 && key==0) SysFieldU32(m, vbuf, vsz, TimeFromBoot(), (uint16_t)DataType::Index, ValueReadOnly);
    else if (field==3 && key==1) SysFieldU32(m, vbuf, vsz, Now(), (uint16_t)DataType::Index, ValueReadOnly);
    else if (field==3 && key==2) SysFieldU32(m, vbuf, vsz, (uint32_t)CurrentTimeOffsetMs(), (uint16_t)DataType::Index, ValueReadOnly);
    else if (field==3 && key==3) SysFieldValue(m, vbuf, vsz, &DeviceStatus.AvgLoopTimeMs, 4, (uint16_t)DataType::Number, ValueReadOnly);
    else if (field==3 && key==4) SysFieldValue(m, vbuf, vsz, &DeviceStatus.MaxLoopTimeMs, 4, (uint16_t)DataType::Number, ValueReadOnly);
    else if (field==4 && key==0) SysFieldU32(m, vbuf, vsz, (uint32_t)GetFreeRAM(), (uint16_t)DataType::Index, ValueReadOnly);
    else if (field==4 && key==1) SysFieldU32(m, vbuf, vsz, GetTotalRAM(), (uint16_t)DataType::Index, ValueReadOnly);
    else if (field==5 && key==0) SysFieldU32(m, vbuf, vsz, Storage.UsedFlashBytes(), (uint16_t)DataType::Index, ValueReadOnly);
    else if (field==5 && key==1) SysFieldU32(m, vbuf, vsz, STORAGE_FLASH_SIZE, (uint16_t)DataType::Index, ValueReadOnly);
    else if (field==6) { m.Type=(uint16_t)DataType::String; m.Flags=ValuePersistent; m.Size=strlen(DeviceName); if(m.Size>16) m.Size=16; vsz=m.Size; memcpy(vbuf, DeviceName, vsz); }
#ifdef TYPE_CORE
    else if (field==7) { uint8_t v=DeviceStatus.NetId; SysFieldValue(m, vbuf, vsz, &v, 1, (uint16_t)DataType::Id, ValuePersistent); }
#endif
#ifdef TYPE_CORE
    else if (field==8 && key==0) {
        // Docs/Services/System Block and Device Commands.md: App Active (No/USB/BLE), RO.
        uint8_t v = AppBLEActive() ? (uint8_t)AppActive::BLE
                  : (AppUSBActive() ? (uint8_t)AppActive::USB
                                    : (uint8_t)AppActive::None);
        SysFieldValue(m, vbuf, vsz, &v, 1, (uint16_t)DataType::Enum, ValueReadOnly);
    }
#endif
    else { return false; }
    return true;
}

static void HandleSystemBlockRead(const PacketFrame &frame, uint32_t bi, uint8_t field, uint8_t key) {
    uint8_t rpl[40]; uint16_t pos=0;
    memcpy(rpl+pos, &bi,4); pos+=4;
    ValueInfo m = {}; uint8_t vsz=0; uint8_t vbuf[24]={0};

    if (field==0xFF) {
        // The same shape as every other block meta (Bi, ValueInfo, 16-char name); the System
        // block has no name of its own.
        SendBlockMetaResponse(frame, bi, (uint16_t)BlockType::System, SYSTEM_FIELD_COUNT, "", 0);
        return;
    }

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

#ifdef USE_DYNAMIC_BLOCKS
static void HandleDynamicBlockRead(const PacketFrame &frame, uint32_t bi, uint16_t inst, uint8_t field, uint8_t key) {
    if (inst >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst);
    if (!block) { RespondStatus(frame,false); return; }
    ReplyDynamicBlockOrField(frame, bi, *block, field, key);
}
#endif

#ifdef USE_SCRIPTS
// Reads one entry of a loaded script (block type 0x3F4-0x3F7) or its block meta. The script's
// input/output/variable/constant tables are addressed as fields 1-4 with key = index.
static void HandleScriptBlockRead(const PacketFrame &frame, uint32_t bi, uint16_t inst, uint8_t field, uint8_t key) {
    LoadedScript *s = ScriptActive(inst);
    if (!s) { RespondStatus(frame,false); return; }
    if (field == 0xFF) {
        SendBlockMetaResponse(frame, bi, BlockTypeRange::ScriptTypeOf(inst), SCRIPT_FIELD_COUNT,
                              s->name, (uint16_t)strlen(s->name));
        return;
    }
    uint8_t rpl[FIELD_RESPONSE_BUF_SIZE];
    uint16_t pos = 0;
    ValueInfo m = {};
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
        SendBlockMetaResponse(frame, bi, (uint16_t)blk.Schema->Type, (uint8_t)blk.Schema->EntryCount,
                              blk.Name, (uint16_t)strlen(blk.Name));
        return;
    }
    FieldResult fr = blk.Get(field);
    if(!fr.Data) { RespondStatus(frame,false); return; }
    SendFieldResponse(frame, bi, fr);
}
