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
inline bool RegisterService::SystemFillValue(uint8_t Field, uint8_t *Vbuf) {
    switch (Field) {
    case 0: { // Device Type struct: DeviceType | Capability | Software version
        uint32_t Dt = (uint32_t)KDeviceType;
        uint32_t Cap = KCapabilities;
        // Software version YY:MM:DD:II (Docs/Services/System Block and Device Commands.md):
        // 7 year + 4 month + 5 day + 16 iteration bits = 32. scripts/version.py is the single
        // source of these field widths (VERSION_FIELDS + pack_version, which mirrors this code);
        // keep the shifts and masks below in lockstep with it.
        static_assert(7 + 4 + 5 + 16 == 32, "System version field widths must fill one u32");
        static_assert(VERSION_YEAR <= 0x7Fu, "version year must fit 7 bits");
        static_assert(VERSION_MONTH <= 0x0Fu, "version month must fit 4 bits");
        static_assert(VERSION_DAY <= 0x1Fu, "version day must fit 5 bits");
        static_assert(VERSION_ITERATION <= 0xFFFFu, "version iteration must fit 16 bits");
        uint32_t Ver = ((uint32_t)(VERSION_YEAR & 0x7Fu) << 25) |
                       ((uint32_t)(VERSION_MONTH & 0x0Fu) << 21) |
                       ((uint32_t)(VERSION_DAY & 0x1Fu) << 16) |
                       ((uint32_t)(VERSION_ITERATION & 0xFFFFu));
        memcpy(Vbuf, &Dt, 4);
        memcpy(Vbuf + 4, &Cap, 4);
        memcpy(Vbuf + 8, &Ver, 4);
        break;
    }
    case 1:
        memcpy(Vbuf, GetSerialNumber().Bytes, 14);
        break;
    case 2: {
        uint16_t Addr = DeviceStatus.ShortAddress;
        memcpy(Vbuf, &Addr, 2);
        break;
    }
    case 3: { // Time struct: Uptime | Current time | Time offset | Loop time | Max Loop time
        uint32_t Up = TimeFromBoot();
        uint32_t NowMs = Now();
        int32_t Off = CurrentTimeOffsetMs();
        memcpy(Vbuf, &Up, 4);
        memcpy(Vbuf + 4, &NowMs, 4);
        memcpy(Vbuf + 8, &Off, 4);
        memcpy(Vbuf + 12, &DeviceStatus.AvgLoopTimeMs, 4);
        memcpy(Vbuf + 16, &DeviceStatus.MaxLoopTimeMs, 4);
        break;
    }
    case 4: { // RAM struct: Used | Total
        uint32_t Total = GetTotalRAM();
        int32_t FreeRam = GetFreeRAM();
        // The field is labelled Used|Total and shown as "Used RAM": report the used bytes,
        // clamped to zero when the free count is bogus/negative or exceeds the total.
        int32_t Used = (FreeRam >= 0 && (uint32_t)FreeRam < Total)
                           ? (int32_t)(Total - (uint32_t)FreeRam)
                           : 0;
        memcpy(Vbuf, &Used, 4);
        memcpy(Vbuf + 4, &Total, 4);
        break;
    }
    case 5: { // FLASH struct: Used | Total
        uint32_t Used = Storage.UsedFlashBytes();
        uint32_t Total = STORAGE_FLASH_SIZE;
        memcpy(Vbuf, &Used, 4);
        memcpy(Vbuf + 4, &Total, 4);
        break;
    }
#ifdef TYPE_CORE
    case 8:
        // Docs/Services/System Block and Device Commands.md: App Active (No/USB/BLE), RO.
        Vbuf[0] = AppBLEActive() ? (uint8_t)AppActive::BLE
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
inline void* RegisterService::SystemGet(uint16_t Field, uint8_t Key)
{
    (void)Key; // the struct position is not addressed on the wire
    if (Field == SYSTEM_FIELD_NAME) return nullptr; // storage-backed (fixed 16-char field)
#ifdef TYPE_CORE
    if (Field == SYSTEM_FIELD_NETID)
    {
        // The live net (the stored value applies on reboot); the write stores it via a trigger.
        Register.NetId = DeviceStatus.NetId;
        return &Register.NetId;
    }
#endif
    if (!SystemFillValue((uint8_t)Field, Register.SysValueBuf)) return nullptr;
    return Register.SysValueBuf;
}

#ifdef TYPE_CORE
// Docs: NetID 0 is unassigned and 0x3F is all-nets, so neither is a valid stored value. The
// write stores it (applied on reboot) without touching the live DeviceStatus.NetId.
inline bool RegisterService::OnSystemNetIdWrite(const StaticBlockDescriptor &Block, uint16_t Field, const void *Data, uint16_t Len) {
    (void)Block; (void)Field;
    if (Len < 1) return false;
    const uint8_t Value = *static_cast<const uint8_t *>(Data);
    if (Value == 0 || Value >= 0x3F) return false;
    memcpy(&StaticPer.System.NetId, Data, 1);
    return true;
}
static const BlockTrigger SystemTriggers[] = { { MakeFieldKey(SYSTEM_FIELD_NETID, 0), RegisterService::OnSystemNetIdWrite } };
#endif

static const BlockSchema SystemSchema = {
    SystemEntries, SystemEntryCount,
#ifdef TYPE_CORE
    SystemTriggers, (uint16_t)(sizeof(SystemTriggers) / sizeof(SystemTriggers[0])),
#else
    nullptr, 0,
#endif
    BlockType::System
};
static const StaticBlockDescriptor SystemBlock = {
    nullptr, &StaticPer, &SystemSchema, "", RegisterService::SystemGet
};

// The descriptor for a (type, instance): the System block (type 0, inst 0) or a static registry
// entry; nullptr when neither exists.
inline const StaticBlockDescriptor* RegisterService::FindBlock(uint16_t Type, uint8_t Inst) {
    if (Type == 0 && Inst == 0) return &SystemBlock;
    int Idx = FindStaticBlock(Type, Inst);
    return Idx >= 0 ? &StaticBlockRegistry[Idx] : nullptr;
}

#ifdef USE_DYNAMIC_BLOCKS
inline void RegisterService::HandleDynamicBlockRead(const PacketFrame &Frame, uint32_t Bi, uint16_t Inst, uint8_t Field, uint8_t Key) {
    if (Inst >= DynamicBlockRegistry.BlockCount) { RespondStatus(Frame,false); return; }
    DynamicBlockDescriptor *Block = DynamicBlockRegistry.GetBlock(Inst);
    if (!Block) { RespondStatus(Frame,false); return; }
    ReplyDynamicBlockOrField(Frame, Bi, *Block, Field, Key);
}
#endif

#ifdef USE_SCRIPTS
// Reads one entry of a loaded script (block type 0x3F4-0x3F7) or its block meta. The script's
// input/output/variable/constant tables are addressed as fields 1-4 with key = index.
inline void RegisterService::HandleScriptBlockRead(const PacketFrame &Frame, uint32_t Bi, uint16_t Inst, uint8_t Field, uint8_t Key) {
    LoadedScript *Loaded = Script.Active(Inst);
    if (!Loaded) { RespondStatus(Frame,false); return; }
    if (Field == 0xFF) {
        SendBlockMetaResponse(Frame, Bi, BlockTypeRange::ScriptTypeOf(Inst), SCRIPT_FIELD_COUNT,
                              Loaded->Name, (uint16_t)strlen(Loaded->Name));
        return;
    }
    ValueInfo Meta = {};
    uint8_t Vbuf[FIELD_RESPONSE_BUF_SIZE];
    uint8_t Vsz = 0;
    if (!Script.GetEntry(Inst, Field, Key, Meta, Vbuf, Vsz)) { RespondStatus(Frame,false); return; }
    FieldResult Fr;
    Fr.Descriptor = Meta;
    Fr.Data = Vbuf;
    SendFieldResponse(Frame, Bi, Fr);
}
#endif

inline void RegisterService::HandleStaticBlockRead(const PacketFrame &Frame, uint32_t Bi, uint16_t Type, uint8_t Inst, uint8_t Field, uint8_t Key) {
    const StaticBlockDescriptor *Blk = FindBlock(Type, Inst);
    if (!Blk) { RespondStatus(Frame,false); return; }
    if (Field==0xFF) { // block meta
        SendBlockMetaResponse(Frame, Bi, (uint16_t)Blk->Schema->Type, (uint8_t)Blk->Schema->EntryCount,
                              Blk->Name, (uint16_t)strlen(Blk->Name));
        return;
    }
    FieldResult Fr = Blk->Get(Field, Key);
    if(!Fr.Data) { RespondStatus(Frame,false); return; }
    SendFieldResponse(Frame, Bi, Fr);
}
