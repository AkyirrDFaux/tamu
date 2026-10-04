#pragma once

// CID 2: read helpers (System, multi-entry, static, dynamic, script).
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 2: Read helpers =====

// Fills the value bytes for one System-block field (type 0, inst 0). The descriptor comes from
// System_Entries (the metadata is unified) and the caller has already resolved the entry, so this
// only synthesises the value. The struct fields (0, 3, 4, 5) fill the whole struct; the struct
// position is not on the wire.
static bool SystemFillValue(uint8_t field, uint8_t *vbuf) {
    switch (field) {
    case 0: { // Device Type struct: DeviceType | Capability | Software version
        uint32_t dt = (uint32_t)kDeviceType;
        uint32_t cap = kCapabilities;
        // Software version YY:MM:DD:II (Docs/Services/System Block and Device Commands.md):
        // 7 year + 4 month + 5 day + 16 iteration bits = 32. scripts/version.py is the single
        // source of these field widths (VERSION_FIELDS + pack_version, which mirrors this code);
        // keep the shifts and masks below in lockstep with it.
        static_assert(7 + 4 + 5 + 16 == 32, "System version field widths must fill one u32");
        static_assert(VERSION_YEAR <= 0x7Fu, "version year must fit 7 bits");
        static_assert(VERSION_MONTH <= 0x0Fu, "version month must fit 4 bits");
        static_assert(VERSION_DAY <= 0x1Fu, "version day must fit 5 bits");
        static_assert(VERSION_ITERATION <= 0xFFFFu, "version iteration must fit 16 bits");
        uint32_t ver = ((uint32_t)(VERSION_YEAR & 0x7Fu) << 25) |
                       ((uint32_t)(VERSION_MONTH & 0x0Fu) << 21) |
                       ((uint32_t)(VERSION_DAY & 0x1Fu) << 16) |
                       ((uint32_t)(VERSION_ITERATION & 0xFFFFu));
        memcpy(vbuf, &dt, 4);
        memcpy(vbuf + 4, &cap, 4);
        memcpy(vbuf + 8, &ver, 4);
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
        uint32_t total = GetTotalRAM();
        int32_t free_ram = GetFreeRAM();
        // The field is labelled Used|Total and shown as "Used RAM": report the used bytes,
        // clamped to zero when the free count is bogus/negative or exceeds the total.
        int32_t used = (free_ram >= 0 && (uint32_t)free_ram < total)
                           ? (int32_t)(total - (uint32_t)free_ram)
                           : 0;
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
#ifdef TYPE_CORE
    case 8:
        // Docs/Services/System Block and Device Commands.md: App Active (No/USB/BLE), RO.
        vbuf[0] = AppBLEActive() ? (uint8_t)AppActive::BLE
                : (AppUSBActive() ? (uint8_t)AppActive::USB : (uint8_t)AppActive::None);
        break;
#endif
    default:
        return false;
    }
    return true;
}

// The System block as a static descriptor: System_Entries is the schema, Name/NetID are the
// storage-backed persistent fields, and the computed fields come from SystemGet.
static void* SystemGet(uint16_t field, uint8_t key) {
    (void)key; // the struct position is not addressed on the wire
    if (field == SYSTEM_FIELD_NAME) return nullptr; // storage-backed (fixed 16-char field)
#ifdef TYPE_CORE
    if (field == SYSTEM_FIELD_NETID) {
        // The live net (the stored value applies on reboot); the write stores it via a trigger.
        static uint8_t s_netId;
        s_netId = DeviceStatus.NetId;
        return &s_netId;
    }
#endif
    static uint8_t s_sysValueBuf[24];
    if (!SystemFillValue((uint8_t)field, s_sysValueBuf)) return nullptr;
    return s_sysValueBuf;
}

#ifdef TYPE_CORE
// Docs: NetID 0 is unassigned and 0x3F is all-nets, so neither is a valid stored value. The
// write stores it (applied on reboot) without touching the live DeviceStatus.NetId.
static bool OnSystemNetIdWrite(const StaticBlockDescriptor &block, uint16_t field, const void *data, uint16_t len) {
    (void)block; (void)field;
    if (len < 1) return false;
    const uint8_t v = *static_cast<const uint8_t *>(data);
    if (v == 0 || v >= 0x3F) return false;
    memcpy(&staticPer.system.NetId, data, 1);
    return true;
}
static const BlockTrigger System_Triggers[] = { { MakeFieldKey(SYSTEM_FIELD_NETID, 0), OnSystemNetIdWrite } };
#endif

static const BlockSchema System_Schema = {
    System_Entries, System_EntryCount,
#ifdef TYPE_CORE
    System_Triggers, (uint16_t)(sizeof(System_Triggers) / sizeof(System_Triggers[0])),
#else
    nullptr, 0,
#endif
    BlockType::System
};
static const StaticBlockDescriptor System_Block = {
    nullptr, &staticPer, &System_Schema, "", SystemGet
};

// The descriptor for a (type, instance): the System block (type 0, inst 0) or a static registry
// entry; nullptr when neither exists.
static const StaticBlockDescriptor* FindBlock(uint16_t type, uint8_t inst) {
    if (type == 0 && inst == 0) return &System_Block;
    int idx = FindStaticBlock(type, inst);
    return idx >= 0 ? &static_block_registry[idx] : nullptr;
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
    ValueInfo m = {};
    uint8_t vbuf[FIELD_RESPONSE_BUF_SIZE];
    uint8_t vsz = 0;
    if (!ScriptGetEntry(inst, field, key, m, vbuf, vsz)) { RespondStatus(frame,false); return; }
    FieldResult fr;
    fr.Descriptor = m;
    fr.Data = vbuf;
    SendFieldResponse(frame, bi, fr);
}
#endif

static void HandleStaticBlockRead(const PacketFrame &frame, uint32_t bi, uint16_t type, uint8_t inst, uint8_t field, uint8_t key) {
    const StaticBlockDescriptor *blk = FindBlock(type, inst);
    if (!blk) { RespondStatus(frame,false); return; }
    if (field==0xFF) { // block meta
        SendBlockMetaResponse(frame, bi, (uint16_t)blk->Schema->Type, (uint8_t)blk->Schema->EntryCount,
                              blk->Name, (uint16_t)strlen(blk->Name));
        return;
    }
    FieldResult fr = blk->Get(field, key);
    if(!fr.Data) { RespondStatus(frame,false); return; }
    SendFieldResponse(frame, bi, fr);
}
