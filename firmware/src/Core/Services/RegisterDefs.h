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
inline uint16_t BlockInfoType(uint32_t bi) { return (bi >> 22) & 0x3FF; }
inline uint8_t BlockInfoInstance(uint32_t bi) { return (bi >> 16) & 0x3F; }
inline uint8_t BlockInfoField(uint32_t bi) { return (bi >> 8) & 0xFF; }
inline uint8_t BlockInfoKey(uint32_t bi) { return bi & 0xFF; }

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

inline bool IsDynamic(uint16_t type) { return type >= DynamicBase && type < DynamicBase + DynamicCount; }
inline bool IsScript(uint16_t type)  { return type >= ScriptBase  && type < ScriptBase  + ScriptCount; }
inline uint16_t BankOf(uint16_t type) { return (uint16_t)(type & (DynamicCount - 1)); }
inline uint16_t DynamicType(uint16_t bank) { return (uint16_t)(DynamicBase + (bank & (DynamicCount - 1))); }
inline uint16_t ScriptType(uint16_t bank)  { return (uint16_t)(ScriptBase  + (bank & (ScriptCount - 1))); }
inline uint16_t GlobalIndex(uint16_t bank, uint8_t instance) { return (uint16_t)((bank << BankShift) | (instance & 0x3F)); }

// The single global dynamic index (0..255) addressed by a banked dynamic BlockInfo.
inline uint16_t DynamicGlobal(uint16_t type, uint8_t instance) { return GlobalIndex(BankOf(type), instance); }
// The single global script index (0..255) addressed by a banked script BlockInfo.
inline uint16_t ScriptGlobal(uint16_t type, uint8_t instance) { return GlobalIndex(type - ScriptBase, instance); }
// The banked script block type that owns global index `global`.
inline uint16_t ScriptTypeOf(uint16_t global) { return ScriptType((uint16_t)(global >> BankShift)); }
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
// Buffer size for field responses: 4 (BlockInfo) + 4 (ValueInfo) + max field size + padding.
// Dynamic (keyed) fields hold concatenated dictionary entries and can reach the u8 size
// limit of ValueInfo.Size (255 bytes), so the response buffer must cover 4+4+255+pad.
// SCALAR_ONLY targets (DAS) carry no Vector/Matrix fields; the largest field is a 16-byte string.
#ifdef SCALAR_ONLY
#define FIELD_RESPONSE_BUF_SIZE 32
#else
#define FIELD_RESPONSE_BUF_SIZE 268
#endif

// Builds a `BlockInfo + ValueInfo + value` reply, 4-byte aligned, and sends it. The block
// meta (a fixed BLOCK_NAME_LEN-char name) and the field/key read paths all share this shape.
static inline void SendFieldLikeResponse(const PacketFrame &frame, uint32_t bi, const ValueInfo &v,
                                         const uint8_t *data, uint16_t len) {
    uint8_t rpl[FIELD_RESPONSE_BUF_SIZE]; uint16_t pos = 0;
    memcpy(rpl + pos, &bi, 4); pos += 4;
    memcpy(rpl + pos, &v, 4); pos += 4;
    if (data && len) memcpy(rpl + pos, data, len);
    pos += len;
    while (pos % 4) rpl[pos++] = 0;
    SendResponse(frame, rpl, pos);
}

static inline void SendBlockMetaResponse(const PacketFrame &frame, uint32_t bi, uint16_t type,
                                         uint8_t map_count, const char *name, uint16_t name_len) {
    // A block meta reports its field count in ValueInfo.Size and carries no flags; the name is
    // a fixed BLOCK_NAME_LEN-char field (space-padded), not a C string.
    char padded[BLOCK_NAME_LEN];
    SetBlockName(padded, name, name_len);
    ValueInfo v = { (uint16_t)(type & 0x3FF), map_count, 0 };
    SendFieldLikeResponse(frame, bi, v, (const uint8_t *)padded, BLOCK_NAME_LEN);
}

static inline void SendFieldResponse(const PacketFrame &frame, uint32_t bi, const FieldResult &fr) {
    SendFieldLikeResponse(frame, bi, fr.Descriptor, (const uint8_t *)fr.Data, fr.Descriptor.Size);
}

// Sends one dynamic entry (BlockInfo echo + ValueInfo + value, 4-aligned).
static inline void SendKeyResponse(const PacketFrame &frame, uint32_t bi, const KeyResult &kr) {
    SendFieldLikeResponse(frame, bi, kr.meta, (const uint8_t *)kr.data_ptr, kr.data_len);
}


#ifdef USE_DYNAMIC_BLOCKS
// Shared tail of "read a dynamic block": field 0xFF asks for the block meta, any other field
// for one keyed entry.
static void ReplyDynamicBlockOrField(const PacketFrame &frame, uint32_t bi,
                                     DynamicBlockDescriptor &block, uint8_t field, uint8_t key)
{
    if (field == 0xFF) {
        // The block's bank type is derived from its global index, which the request's
        // BlockInfo carries; a tombstone reports None (Docs "Dynamic Block Descriptor").
        uint16_t type = block.present ? BlockInfoType(bi) : (uint16_t)BlockType::None;
        SendBlockMetaResponse(frame, bi, type, (uint8_t)block.FieldCount(), block.Name, BLOCK_NAME_LEN);
        return;
    }
    KeyResult kr = block.GetKey(field, key);
    if (!kr.exists) { RespondStatus(frame, false); return; }
    SendKeyResponse(frame, bi, kr);
}
#endif
