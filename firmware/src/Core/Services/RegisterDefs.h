#pragma once

// BlockInfo accessors and the shared response helpers.
//
// Part of Core/Services/Register.h (included from there).


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

static inline void SendFieldResponse(const PacketFrame &frame, uint32_t bi, const FieldResult &fr,
                                     uint16_t activeFlags = 0) {
    uint8_t rpl[FIELD_RESPONSE_BUF_SIZE]; uint16_t pos=0;
    memcpy(rpl+pos, &bi,4); pos+=4;
    // Active flags are not part of the (const) schema: fold them into the reported meta.
    BlockMeta m = fr.Descriptor;
    m.FlagsAndType |= activeFlags;
    memcpy(rpl+pos, &m,4); pos+=4;
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


// The specification of a write is *declared by the writer* in the write's ValueInfo flags
// (Docs/Services/Register.md: "Script Updated = a script changed this variable", "External
// origin = a subscription is updating this variable or a different device updated this via
// script"). A write that declares neither is a plain manual write, so a write of a different
// specification clears the origin flags it does not carry (user decision). The receiving side
// never guesses from the sender's address.
static inline WriteOrigin OriginFromDeclaredFlags(uint16_t flagsAndType)
{
    if (flagsAndType & (uint16_t)FieldFlags::ScriptUpdated) return OriginLocalScript;
    if (flagsAndType & (uint16_t)FieldFlags::External) return OriginForeign;
    return OriginManual;
}

// Stores a static block's write-origin flags from the specification the writer declared.
static inline void StaticMarkWriteFromFlags(uint8_t idx, uint8_t field, uint16_t flagsAndType)
{
    StaticActiveMarkWrite(idx, field, OriginFromDeclaredFlags(flagsAndType));
}

#ifndef DISABLE_DYNAMIC_MEMORY
// Shared tail of "read a dynamic block": field 0xFF asks for the block meta, any other field
// for one keyed entry. Both the live read and the backup read reply this same way, they only
// differ in where the descriptor comes from.
static void ReplyDynamicBlockOrField(const PacketFrame &frame, uint32_t bi,
                                     DynamicBlockDescriptor &block, uint8_t field, uint8_t key)
{
    if (field == 0xFF) {
        SendBlockMetaResponse(frame, bi, (uint16_t)block.type, (uint8_t)block.FieldCount(), block.Name);
        return;
    }
    KeyResult kr = block.GetKey(field, key);
    if (!kr.exists) { RespondStatus(frame, false); return; }
    SendKeyResponse(frame, bi, kr);
}
#endif
