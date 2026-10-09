#pragma once

// CID 0/1: enumerate blocks, fields and keys.
//
// Part of Core/Services/Register.h (included from there).
//
// Two requests, both streamed as 64-byte FRAG fragments (the shape Storage's reads use, which
// the app already reassembles):
//   (empty)                      -> the present block types, one packed word each:
//                                   (type << 6) | maximum instance
//                                   A type with no instances is omitted (dynamic memory with no
//                                   blocks); scripts are omitted too - the app reads their loaded
//                                   slots from the Script service's CID 0.
//   (type << 6) | instance       -> that block's Field&Key words (field << 8 | key), ascending.
//                                   Static and System blocks share their type's schema, so their
//                                   list is type-invariant (the instance is accepted and
//                                   ignored, and the app caches it per type); dynamic blocks and
//                                   loaded scripts are per instance.
//
// Every list kind is described by one `EnumSrc` tag plus a context pointer and routed through a
// *single* streaming loop. That matters on the 16 KB CH32 node: a per-kind template would inline
// the whole fragment loop (and its word generator) once per call site, which measured at over a
// kilobyte of flash for what is a handful of words per list.

#include "Core/Services/RegisterDefs.h"

// The System block's schema (Docs/Services/System Block and Device Commands.md). One entry per
// field, key 0: the struct fields (0, 3, 4, 5) are Undefined raw bytes whose Size is the sum of
// their members. The struct position is never addressed on the wire - the whole struct is sent.
static const BlockEntry System_Entries[] = {
    { MakeFieldKey(0, 0), 0,  { (uint16_t)DataType::Undefined, 12, ValueReadOnly } }, // DeviceType + Capability + Version
    { MakeFieldKey(1, 0), 0,  { (uint16_t)DataType::SN,        14, ValueReadOnly } },
    { MakeFieldKey(2, 0), 0,  { (uint16_t)DataType::Id,         2, ValueReadOnly } },
    { MakeFieldKey(3, 0), 0,  { (uint16_t)DataType::Undefined, 20, ValueReadOnly } }, // Uptime + Now + Offset + AvgLoop + MaxLoop
    { MakeFieldKey(4, 0), 0,  { (uint16_t)DataType::Undefined,  8, ValueReadOnly } }, // UsedRAM + TotalRAM
    { MakeFieldKey(5, 0), 0,  { (uint16_t)DataType::Undefined,  8, ValueReadOnly } }, // UsedFlash + TotalFlash
    { MakeFieldKey(6, 0), 0,  { (uint16_t)DataType::Name,      16, ValuePersistent } }, // Name
#ifdef TYPE_CORE
    { MakeFieldKey(7, 0), 16, { (uint16_t)DataType::Id,         1, ValuePersistent } }, // NetID
    { MakeFieldKey(8, 0), 0,  { (uint16_t)DataType::Enum,       1, ValueReadOnly } },   // App Active
#endif
};
static const uint16_t System_EntryCount = sizeof(System_Entries) / sizeof(System_Entries[0]);

// Which list is being streamed.
enum class EnumSrc : uint8_t { System, Static, Dynamic, Script };

// The highest instance a 6-bit packed type word can carry. A type with more instances than
// that cannot be addressed by BlockInfo anyway, so the packed highest instance must saturate
// rather than truncate silently (the old `& 0x3F` wrapped a 64th instance back to 0).
#define ENUM_MAX_INSTANCE 0x3F

// Packs a normal (10.6) block-type word, saturating the instance field.
static inline uint16_t EnumTypeWordOf(uint16_t type, size_t instances) {
    uint16_t inst = instances > ENUM_MAX_INSTANCE ? ENUM_MAX_INSTANCE : (uint16_t)instances;
    return (uint16_t)((type << 6) | inst);
}

static uint16_t EnumTypeCount() {
    uint16_t count = 1; // the System block
    for (size_t i = 0; i < static_block_num; i++)
        if (i == 0 || static_block_registry[i].Schema->Type != static_block_registry[i - 1].Schema->Type)
            count++;
#ifdef USE_DYNAMIC_BLOCKS
    if (dynamic_block_registry.block_count > 0) count++;
#endif
    return count;
}

#ifdef USE_SCRIPTS
static uint16_t EnumScriptWord(uint16_t inst, uint16_t i) {
    uint16_t seen = 0;
    for (uint16_t f = 0; f <= SCRIPT_FIELD_OUTPUT; f++) {
        uint8_t keys = ScriptKeyCount(inst, (uint8_t)f);
        for (uint8_t k = 0; k < keys; k++)
            if (seen++ == i) return (uint16_t)((f << 8) | k);
    }
    return 0;
}
#endif

// The word at index `i` of the requested list, or 0 past its end. Out of line so LTO cannot
// clone the streamer's loop once per list kind (which re-inflates it to the pre-refactor size).
__attribute__((noinline)) static uint16_t EnumWord(EnumSrc src, const void *ctx, uint16_t i) {
    switch (src) {
    case EnumSrc::System:
        return System_Entries[i].FieldKey;
    case EnumSrc::Static: {
        const BlockSchema *s = (const BlockSchema *)ctx;
        return s->Entries[i].FieldKey; // the literal table carries the Field&Key
    }
#ifdef USE_DYNAMIC_BLOCKS
    case EnumSrc::Dynamic: {
        const DynamicBlockDescriptor *b = (const DynamicBlockDescriptor *)ctx;
        return b->table[i].fieldKey;
    }
#endif
#ifdef USE_SCRIPTS
    case EnumSrc::Script:
        return EnumScriptWord(*(const uint16_t *)ctx, i);
#endif
    default:
        break;
    }
    return 0;
}

// Streams `count` words as FRAG fragments, one lookup at a time - no buffer, the content goes
// straight into the frame payload the send path already has. `count` may be 0 (an empty reply).
static void SendU16Stream(const PacketFrame &frame, uint16_t count, EnumSrc src, const void *ctx) {
    const uint16_t kWordsPerFrag = 32; // 64-byte fragments, two bytes per word
    uint16_t frags = (uint16_t)((count + kWordsPerFrag - 1) / kWordsPerFrag);
    if (frags == 0) frags = 1;

    uint16_t idx = 0; // the next word to emit
    for (uint16_t f = 0; f < frags; f++) {
        uint8_t *dst = tx_frame.payload + 4;
        uint16_t n = 0;
        while (n < kWordsPerFrag * 2 && idx < count) {
            uint16_t w = EnumWord(src, ctx, idx++);
            dst[n++] = (uint8_t)w;
            dst[n++] = (uint8_t)(w >> 8);
        }
        SendFragFragment(frame, f, frags, n);
    }
}

// CID 0: the present block types, one packed block-type word each. The list is generated
// during the fragment loop in a single pass over the grouped static registry (the old
// per-index lookup rescanned the registry for every emitted word, O(types x blocks)); the
// count pass only sizes the reply. The System block is always first: its word is the only
// zero and it is first, which is what lets the app tell a real zero from the wire's trailing
// padding.
static void SendTypeWordsStream(const PacketFrame &frame) {
    const uint16_t kWordsPerFrag = 32; // 64-byte fragments, two bytes per word
    uint16_t count = EnumTypeCount();
    uint16_t frags = (uint16_t)((count + kWordsPerFrag - 1) / kWordsPerFrag);
    if (frags == 0) frags = 1;

    size_t start = 0;   // the first registry entry of the current type run
    bool systemSent = false;
    uint16_t idx = 0;   // the next word to emit

    for (uint16_t f = 0; f < frags; f++) {
        uint8_t *dst = tx_frame.payload + 4;
        uint16_t n = 0;
        while (n < kWordsPerFrag * 2 && idx < count) {
            uint16_t w = 0;
            if (!systemSent) {
                systemSent = true; // word 0: the System block (type 0, instance 0)
            } else if (start < static_block_num) {
                uint16_t t = (uint16_t)static_block_registry[start].Schema->Type;
                size_t end = start + 1;
                while (end < static_block_num &&
                       (uint16_t)static_block_registry[end].Schema->Type == t) end++;
                w = EnumTypeWordOf(t, end - start - 1);
                start = end;
            }
#ifdef USE_DYNAMIC_BLOCKS
            else if (dynamic_block_registry.block_count > 0) {
                // Doc gap: Register.md "Block types" does not describe the banked dynamic
                // range's **8.8** encoding (high byte = the owning bank type's low byte,
                // 0xF0-0xF3; low byte = the highest occupied global index, 0..255), which
                // differs from the normal 10.6 word. It lives only in this code; tracked in
                // Issues.md. `count` includes exactly one such word.
                uint16_t highest = (uint16_t)(dynamic_block_registry.block_count - 1);
                uint16_t bankType = BlockTypeRange::DynamicType((uint16_t)(highest >> BlockTypeRange::BankShift));
                w = (uint16_t)((bankType << 8) | (highest & 0xFF));
            }
#endif
            dst[n++] = (uint8_t)w;
            dst[n++] = (uint8_t)(w >> 8);
            idx++;
        }
        SendFragFragment(frame, f, frags, n);
    }
}

static void HandleEnumerateBlocks(const PacketFrame &frame) {
    SendTypeWordsStream(frame);
}

// CID 1: a block's Field&Key words. The request carries the packed `(type << 6) | instance`
// word (a uint16 on the wire). An absent or tombstoned block reports an empty list rather than a
// failure.
static void HandleEnumerateFields(const PacketFrame &frame) {
    if (PayloadBytes(frame) != 4) { RespondStatus(frame, false); return; }
    uint16_t packed = (uint16_t)(frame.payload[0] | (frame.payload[1] << 8));
    uint16_t type = (packed >> 6) & 0x3FF;
    uint8_t inst = (uint8_t)(packed & 0x3F);
    uint16_t gi = 0; // the global index of a banked (script/dynamic) block; ctx points at it
    (void)inst; (void)gi; // unused when neither USE_SCRIPTS nor USE_DYNAMIC_BLOCKS is set
    EnumSrc src = EnumSrc::System; // overridden below; unused for a zero-count reply
    const void *ctx = nullptr;
    uint16_t count = 0;

    if (type == 0) { // the System block: virtual, its schema is the fixed System_Entries
        src = EnumSrc::System;
        count = System_EntryCount;
#ifdef USE_SCRIPTS
    } else if (BlockTypeRange::IsScript(type)) { // a loaded script: its inputs and outputs, per instance
        gi = BlockTypeRange::ScriptGlobal(type, inst);
        LoadedScript *s = ScriptActive(gi);
        if (s) {
            src = EnumSrc::Script;
            ctx = &gi;
            count = (uint16_t)(s->inCount + s->outCount);
        }
#endif
#ifdef USE_DYNAMIC_BLOCKS
    } else if (BlockTypeRange::IsDynamic(type)) { // a dynamic block: its live entries, per instance
        gi = BlockTypeRange::DynamicGlobal(type, inst);
        DynamicBlockDescriptor *b = gi < dynamic_block_registry.block_count
                                        ? dynamic_block_registry.GetBlock(gi)
                                        : nullptr;
        if (b && b->present) {
            src = EnumSrc::Dynamic;
            ctx = b;
            count = b->entry_count;
        }
#endif
    } else { // a static registry entry: the schema is the type's, every instance answers alike
        int idx = FindStaticBlock(type, 0);
        if (idx >= 0) {
            src = EnumSrc::Static;
            ctx = static_block_registry[idx].Schema;
            count = static_block_registry[idx].Schema->EntryCount;
        }
    }
    SendU16Stream(frame, count, src, ctx);
}
