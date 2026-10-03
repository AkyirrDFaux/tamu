#pragma once

// CID 1: read helpers (System, multi-entry, static, dynamic, script).
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 1: Read helpers =====

// Resolves one System-block field (type 0, inst 0) into a descriptor + value. The descriptor
// comes from System_Entries (the metadata is unified); this only fills the value bytes. The
// struct fields (0, 3, 4, 5) return the whole struct; the struct position is not on the wire.
bool RegisterGetSystemField(uint8_t field, uint8_t key, ValueInfo &m, uint8_t *vbuf, uint8_t &vsz) {
    (void)key; // the struct position is not addressed on the wire
    const BlockEntry *e = nullptr;
    for (uint16_t i = 0; i < System_EntryCount; i++)
        if (FieldOf(System_Entries[i].FieldKey) == field) { e = &System_Entries[i]; break; }
    if (!e) return false;
    m = e->Info;

    switch (field) {
    case 0: { // Device Type struct: DeviceType | Capability | Software version
        uint32_t dt = (uint32_t)kDeviceType;
        uint32_t cap = kCapabilities;
        uint8_t ver[4] = { (uint8_t)(VERSION_YEAR % 100), VERSION_MONTH, VERSION_DAY, VERSION_ITERATION };
        memcpy(vbuf, &dt, 4);
        memcpy(vbuf + 4, &cap, 4);
        memcpy(vbuf + 8, ver, 4);
        break;
    }
    case 1:
        memcpy(vbuf, GetSerialNumber().bytes, 14);
        break;
    case 2: {
        uint16_t v = DeviceStatus.ShortAddress;
        memcpy(vbuf, &v, 2);
        break;
    }
    case 3: { // Time struct: Uptime | Current time | Time offset | Loop time | Max Loop time
        uint32_t up = TimeFromBoot();
        uint32_t now = Now();
        int32_t off = CurrentTimeOffsetMs();
        memcpy(vbuf, &up, 4);
        memcpy(vbuf + 4, &now, 4);
        memcpy(vbuf + 8, &off, 4);
        memcpy(vbuf + 12, &DeviceStatus.AvgLoopTimeMs, 4);
        memcpy(vbuf + 16, &DeviceStatus.MaxLoopTimeMs, 4);
        break;
    }
    case 4: { // RAM struct: Used | Total
        int32_t used = GetFreeRAM();
        uint32_t total = GetTotalRAM();
        memcpy(vbuf, &used, 4);
        memcpy(vbuf + 4, &total, 4);
        break;
    }
    case 5: { // FLASH struct: Used | Total
        uint32_t used = Storage.UsedFlashBytes();
        uint32_t total = STORAGE_FLASH_SIZE;
        memcpy(vbuf, &used, 4);
        memcpy(vbuf + 4, &total, 4);
        break;
    }
    case 6: // Name (fixed 16-char field, space-padded)
        memcpy(vbuf, staticPer.system.Name, SYSTEM_NAME_LEN);
        break;
#ifdef TYPE_CORE
    case 7:
        vbuf[0] = DeviceStatus.NetId;
        break;
    case 8:
        // Docs/Services/System Block and Device Commands.md: App Active (No/USB/BLE), RO.
        vbuf[0] = AppBLEActive() ? (uint8_t)AppActive::BLE
                : (AppUSBActive() ? (uint8_t)AppActive::USB : (uint8_t)AppActive::None);
        break;
#endif
    default:
        return false;
    }
    vsz = m.Size;
    return true;
}

static void HandleSystemBlockRead(const PacketFrame &frame, uint32_t bi, uint8_t field, uint8_t key) {
    if (field==0xFF) {
        // The same shape as every other block meta (Bi, ValueInfo, 16-char name); the System
        // block has no name of its own.
        SendBlockMetaResponse(frame, bi, (uint16_t)BlockType::System, SYSTEM_FIELD_COUNT, "", 0);
        return;
    }

    // All system fields resolve through the shared RegisterGetSystemField (single source of
    // truth - the Subscriptions service uses the same path), then reply through the same helper
    // the static blocks use.
    ValueInfo m = {}; uint8_t vsz=0; uint8_t vbuf[24]={0};
    if (!RegisterGetSystemField(field, key, m, vbuf, vsz))
    {
        RespondStatus(frame,false);
        return;
    }
    FieldResult fr;
    fr.Descriptor = m;
    fr.Data = vbuf;
    SendFieldResponse(frame, bi, fr);
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

static void HandleStaticBlockRead(const PacketFrame &frame, uint32_t bi, uint16_t type, uint8_t inst, uint8_t field, uint8_t key) {
    int idx = FindStaticBlock(type, inst);
    if (idx < 0) { RespondStatus(frame,false); return; }
    const StaticBlockDescriptor &blk = static_block_registry[idx];
    if (field==0xFF) { // block meta
        SendBlockMetaResponse(frame, bi, (uint16_t)blk.Schema->Type, (uint8_t)blk.Schema->EntryCount,
                              blk.Name, (uint16_t)strlen(blk.Name));
        return;
    }
    FieldResult fr = blk.Get(field, key);
    if(!fr.Data) { RespondStatus(frame,false); return; }
    SendFieldResponse(frame, bi, fr);
}
