#pragma once

// BlockInfo accessors and the shared response helpers.
//
// Part of Core/Services/Register.h (included from there).

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
inline uint16_t BlockInfoType(uint32_t Bi) { return (Bi >> 22) & 0x3FF; }
inline uint8_t BlockInfoInstance(uint32_t Bi) { return (Bi >> 16) & 0x3F; }
inline uint8_t BlockInfoField(uint32_t Bi) { return (Bi >> 8) & 0xFF; }
inline uint8_t BlockInfoKey(uint32_t Bi) { return Bi & 0xFF; }

// ===== Register service command IDs (Docs/Services/Register.md) =====
// Basic commands. Enumerate is split: CID 0 lists the block types, CID 1 a block's Field&Keys.
enum class RegisterCid : uint8_t {
    EnumerateBlocks = 0,
    EnumerateFields = 1,
    Read            = 2,
    Write           = 3,
    RecallAll       = 4,
    SaveAll         = 5,
};

// Dynamic/keyed management commands.
enum class DynamicCid : uint8_t {
    Create  = 0x10,
    Delete  = 0x11,
    GetName = 0x12,
    SetName = 0x13,
};

// ===== Block type ranges (Docs/Services/Register.md "Block types") =====
// Dynamic and Scripts each own four consecutive types ("banks"), 64 instances per type, so a
// global 0..255 index maps to (bank, instance) = (index / 64, index % 64).
namespace BlockTypeRange {
constexpr uint16_t DynamicBase  = 0x3F0;
constexpr uint16_t DynamicCount = 4;
constexpr uint16_t ScriptBase   = 0x3F4;
constexpr uint16_t ScriptCount  = 4;
constexpr uint16_t ReservedBase = 0x3F8;
constexpr uint16_t Instances    = 64; // BlockInstance is 6 bits
constexpr uint8_t  BankShift    = 6;  // instance bits

inline bool IsDynamic(uint16_t Type) { return Type >= DynamicBase && Type < DynamicBase + DynamicCount; }
inline bool IsScript(uint16_t Type)  { return Type >= ScriptBase  && Type < ScriptBase  + ScriptCount; }
inline uint16_t BankOf(uint16_t Type) { return (uint16_t)(Type & (DynamicCount - 1)); }
inline uint16_t DynamicType(uint16_t Bank) { return (uint16_t)(DynamicBase + (Bank & (DynamicCount - 1))); }
inline uint16_t ScriptType(uint16_t Bank)  { return (uint16_t)(ScriptBase  + (Bank & (ScriptCount - 1))); }
inline uint16_t GlobalIndex(uint16_t Bank, uint8_t Instance) { return (uint16_t)((Bank << BankShift) | (Instance & 0x3F)); }

// The single global dynamic index (0..255) addressed by a banked dynamic BlockInfo.
inline uint16_t DynamicGlobal(uint16_t Type, uint8_t Instance) { return GlobalIndex(BankOf(Type), Instance); }
// The single global script index (0..255) addressed by a banked script BlockInfo.
inline uint16_t ScriptGlobal(uint16_t Type, uint8_t Instance) { return GlobalIndex(Type - ScriptBase, Instance); }
// The banked script block type that owns global index `global`.
inline uint16_t ScriptTypeOf(uint16_t Global) { return ScriptType((uint16_t)(Global >> BankShift)); }
}

// Find static block index from Type+Instance.
//
// `inst` is the *per-type ordinal* (the Nth registry entry of this type) and the returned index
// is the array position - the app reconstructs it from enumeration alone, and it has to
// reconstruct from enumeration alone. The app derives the order by enumerating block types
// (first-seen array order) and then instances, so the two agree only while the registry stays
// **grouped by type**. Interleaving types (e.g. Fan1, AccGyr, Fan2) would make the app's
// type-then-instance order differ from the array order and silently mis-name fields;
// the app correspondingly filters Script/Dynamic out of its registry (they are appended after
// the statics, so their presence only *happens* to be harmless).
inline int FindStaticBlock(uint16_t Type, uint8_t Inst) {
    int Count=0;
    for (size_t i=0;i<StaticBlockNum;i++) {
        if ((uint16_t)StaticBlockRegistry[i].Schema->Type == Type) {
            if (Count==Inst) return (int)i;
            Count++;
        }
    }
    return -1;
}

// ===== Helpers for common response patterns =====
// Stack buffer for a field response: BlockInfo(4) + ValueInfo(4) + value + padding. Value
// replies are capped to FIELD_RESPONSE_MAX_VALUE at send time, so the buffer only has to cover
// the descriptor shapes (the meta name, the SCALAR_ONLY string fields); it stays generous for
// the non-SCALAR targets whose callers also use it as a local value buffer.
// SCALAR_ONLY targets (DAS) carry no Vector/Matrix fields; the largest field is a 16-byte string.
#ifdef SCALAR_ONLY
#define FIELD_RESPONSE_BUF_SIZE 32
#else
#define FIELD_RESPONSE_BUF_SIZE 268
#endif

// The largest value a value reply can carry. It is bounded by the packet (BlockInfo(4) +
// ValueInfo(4) + value, payload capped at MAX_PAYLOAD_SIZE) and also by the response buffer
// (SCALAR_ONLY builds a 32-byte one). ValueInfo.Size is 8 bits and a dynamic keyed entry may
// describe more, but a single packet cannot carry it - a value reply caps both the bytes and
// the reported Size together.
#define FIELD_RESPONSE_MAX_VALUE \
    (((MAX_PAYLOAD_SIZE - 8) < (FIELD_RESPONSE_BUF_SIZE - 8)) ? (MAX_PAYLOAD_SIZE - 8) \
                                                              : (FIELD_RESPONSE_BUF_SIZE - 8))

// Builds a `BlockInfo + ValueInfo + value` reply, 4-byte aligned, and sends it. The block
// meta (a fixed BLOCK_NAME_LEN-char name) and the field/key read paths all share this shape.
// `len` is clamped so the raw buffer cannot overrun when a caller reports an oversized value.
inline void RegisterService::SendFieldLikeResponse(const PacketFrame &Frame, uint32_t Bi, const ValueInfo &v,
                                                   const uint8_t *Data, uint16_t Len) {
    if (Len > FIELD_RESPONSE_MAX_VALUE) Len = FIELD_RESPONSE_MAX_VALUE;
    uint8_t Rpl[FIELD_RESPONSE_BUF_SIZE]; uint16_t Pos = 0;
    memcpy(Rpl + Pos, &Bi, 4); Pos += 4;
    memcpy(Rpl + Pos, &v, 4); Pos += 4;
    if (Data && Len) memcpy(Rpl + Pos, Data, Len);
    Pos += Len;
    while (Pos % 4) Rpl[Pos++] = 0;
    SendResponse(Frame, Rpl, Pos);
}

// A field *value* reply (as opposed to the block meta, whose ValueInfo.Size is a field count).
// The value is capped to one packet and the echoed ValueInfo.Size is rewritten to match the
// bytes actually sent, so the reply is self-consistent and a reader can round-trip what it
// received (without this, SendResponse silently truncates an oversized value and the reported
// Size lies about the bytes present).
inline void RegisterService::SendValueResponse(const PacketFrame &Frame, uint32_t Bi, const ValueInfo &v,
                                               const uint8_t *Data, uint16_t Len) {
    if (Len > FIELD_RESPONSE_MAX_VALUE) Len = FIELD_RESPONSE_MAX_VALUE;
    ValueInfo Capped = v;
    Capped.Size = (uint8_t)Len;
    SendFieldLikeResponse(Frame, Bi, Capped, Data, Len);
}

inline void RegisterService::SendBlockMetaResponse(const PacketFrame &Frame, uint32_t Bi, uint16_t Type,
                                                   uint8_t MapCount, const char *Name, uint16_t NameLen) {
    // A block meta reports its field count in ValueInfo.Size and carries no flags; the name is
    // a fixed BLOCK_NAME_LEN-char field (space-padded), not a C string.
    char Padded[BLOCK_NAME_LEN];
    SetBlockName(Padded, Name, NameLen);
    ValueInfo v = { (uint16_t)(Type & 0x3FF), MapCount, 0 };
    SendFieldLikeResponse(Frame, Bi, v, (const uint8_t *)Padded, BLOCK_NAME_LEN);
}

inline void RegisterService::SendFieldResponse(const PacketFrame &Frame, uint32_t Bi, const FieldResult &Fr) {
    SendValueResponse(Frame, Bi, Fr.Descriptor, (const uint8_t *)Fr.Data, Fr.Descriptor.Size);
}

// Sends one dynamic entry (BlockInfo echo + ValueInfo + value, 4-aligned).
inline void RegisterService::SendKeyResponse(const PacketFrame &Frame, uint32_t Bi, const KeyResult &Kr) {
    SendValueResponse(Frame, Bi, Kr.Meta, (const uint8_t *)Kr.DataPtr, Kr.DataLen);
}

// Replies with the assigned BlockIndex followed by an ack (the dynamic field-write path).
inline void RegisterService::SendBlockIndexAck(const PacketFrame &Frame, uint8_t Index) {
    uint8_t Payload[sizeof(BlockIndex) + 1];
    BlockIndex OutIndex = {Index, 0xFF, 0xFF};
    memcpy(Payload, &OutIndex, sizeof(BlockIndex));
    Payload[sizeof(BlockIndex)] = 1;
    SendResponse(Frame, Payload, sizeof(Payload));
}


#ifdef USE_DYNAMIC_BLOCKS
// Distinct field count for a dynamic block's meta reply. The descriptor's `FieldCount()`
// allocates a 256-byte scratch on the stack and rescans; the table is sorted by Field&Key, so
// counting field changes gives the same answer without the buffer. (A cross-call cache would
// need a field on DynamicBlockDescriptor, i.e. MemoryBlocks.h, which is outside this file.)
inline uint16_t RegisterService::DynamicFieldCount(const DynamicBlockDescriptor &b) {
    uint16_t Count = 0, Prev = 0xFFFF;
    for (uint16_t i = 0; i < b.EntryCount; i++) {
        uint16_t f = FieldOf(b.Table[i].FieldKey);
        if (f != Prev) { Count++; Prev = f; }
    }
    return Count;
}

// Shared tail of "read a dynamic block": field 0xFF asks for the block meta, any other field
// for one keyed entry.
inline void RegisterService::ReplyDynamicBlockOrField(const PacketFrame &Frame, uint32_t Bi,
                                                      DynamicBlockDescriptor &Block, uint8_t Field, uint8_t Key)
{
    if (Field == 0xFF) {
        // The block's bank type is derived from its global index, which the request's
        // BlockInfo carries; a tombstone reports None (Docs "Dynamic Block Descriptor").
        uint16_t Type = Block.Present ? BlockInfoType(Bi) : (uint16_t)BlockType::None;
        SendBlockMetaResponse(Frame, Bi, Type, (uint8_t)DynamicFieldCount(Block), Block.Name, BLOCK_NAME_LEN);
        return;
    }
    KeyResult Kr = Block.GetKey(Field, Key);
    if (!Kr.Exists) { RespondStatus(Frame, false); return; }
    SendKeyResponse(Frame, Bi, Kr);
}
#endif
