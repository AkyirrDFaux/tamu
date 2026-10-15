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
static const BlockEntry SystemEntries[] = {
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
static const uint16_t SystemEntryCount = sizeof(SystemEntries) / sizeof(SystemEntries[0]);

// The highest instance a 6-bit packed type word can carry. A type with more instances than
// that cannot be addressed by BlockInfo anyway, so the packed highest instance must saturate
// rather than truncate silently (the old `& 0x3F` wrapped a 64th instance back to 0).
#define ENUM_MAX_INSTANCE 0x3F

// Packs a normal (10.6) block-type word, saturating the instance field.
inline uint16_t RegisterService::EnumTypeWordOf(uint16_t Type, size_t Instances) {
    uint16_t Inst = Instances > ENUM_MAX_INSTANCE ? ENUM_MAX_INSTANCE : (uint16_t)Instances;
    return (uint16_t)((Type << 6) | Inst);
}

inline uint16_t RegisterService::EnumTypeCount() {
    uint16_t Count = 1; // the System block
    for (size_t i = 0; i < StaticBlockNum; i++)
        if (i == 0 || StaticBlockRegistry[i].Schema->Type != StaticBlockRegistry[i - 1].Schema->Type)
            Count++;
#ifdef USE_DYNAMIC_BLOCKS
    if (DynamicBlockRegistry.BlockCount > 0) Count++;
#endif
    return Count;
}

#ifdef USE_SCRIPTS
inline uint16_t RegisterService::EnumScriptWord(uint16_t Inst, uint16_t i) {
    uint16_t Seen = 0;
    for (uint16_t Field = 0; Field <= SCRIPT_FIELD_OUTPUT; Field++) {
        uint8_t Keys = Script.KeyCount(Inst, (uint8_t)Field);
        for (uint8_t k = 0; k < Keys; k++)
            if (Seen++ == i) return MakeFieldKey((uint8_t)Field, k);
    }
    return 0;
}
#endif

// The word at index `i` of the requested list, or 0 past its end. Out of line so LTO cannot
// clone the streamer's loop once per list kind (which re-inflates it to the pre-refactor size).
__attribute__((noinline)) inline uint16_t RegisterService::EnumWord(EnumSrc Src, const void *Ctx, uint16_t i) {
    switch (Src) {
    case EnumSrc::System:
        return SystemEntries[i].FieldKey;
    case EnumSrc::Static: {
        const BlockSchema *Schema = (const BlockSchema *)Ctx;
        return Schema->Entries[i].FieldKey; // the literal table carries the Field&Key
    }
#ifdef USE_DYNAMIC_BLOCKS
    case EnumSrc::Dynamic: {
        const DynamicBlockDescriptor *Block = (const DynamicBlockDescriptor *)Ctx;
        return Block->Table[i].FieldKey;
    }
#endif
#ifdef USE_SCRIPTS
    case EnumSrc::Script:
        return EnumScriptWord(*(const uint16_t *)Ctx, i);
#endif
    default:
        break;
    }
    return 0;
}

// Streams `count` words as FRAG fragments, one lookup at a time - no buffer, the content goes
// straight into the frame payload the send path already has. `count` may be 0 (an empty reply).
inline void RegisterService::SendU16Stream(const PacketFrame &Frame, uint16_t Count, EnumSrc Src, const void *Ctx) {
    uint16_t Frags = (uint16_t)((Count + MAX_FRAG_WORDS - 1) / MAX_FRAG_WORDS);
    if (Frags == 0) Frags = 1;

    uint16_t Idx = 0; // the next word to emit
    for (uint16_t Field = 0; Field < Frags; Field++) {
        uint8_t *Dst = TxFrame.Payload + 4;
        uint16_t Len = 0;
        while (Len < MAX_FRAG_WORD_BYTES && Idx < Count) {
            uint16_t Word = EnumWord(Src, Ctx, Idx++);
            Dst[Len++] = (uint8_t)Word;
            Dst[Len++] = (uint8_t)(Word >> 8);
        }
        SendFragFragment(Frame, Field, Frags, Len);
    }
}

// CID 0: the present block types, one packed block-type word each. The list is generated
// during the fragment loop in a single pass over the grouped static registry (the old
// per-index lookup rescanned the registry for every emitted word, O(types x blocks)); the
// count pass only sizes the reply. The System block is always first: its word is the only
// zero and it is first, which is what lets the app tell a real zero from the wire's trailing
// padding.
inline void RegisterService::SendTypeWordsStream(const PacketFrame &Frame) {
    uint16_t Count = EnumTypeCount();
    uint16_t Frags = (uint16_t)((Count + MAX_FRAG_WORDS - 1) / MAX_FRAG_WORDS);
    if (Frags == 0) Frags = 1;

    size_t Start = 0;   // the first registry entry of the current type run
    bool SystemSent = false;
    uint16_t Idx = 0;   // the next word to emit

    for (uint16_t Field = 0; Field < Frags; Field++) {
        uint8_t *Dst = TxFrame.Payload + 4;
        uint16_t Len = 0;
        while (Len < MAX_FRAG_WORD_BYTES && Idx < Count) {
            uint16_t Word = 0;
            if (!SystemSent) {
                SystemSent = true; // word 0: the System block (type 0, instance 0)
            } else if (Start < StaticBlockNum) {
                uint16_t Type = (uint16_t)StaticBlockRegistry[Start].Schema->Type;
                size_t End = Start + 1;
                while (End < StaticBlockNum &&
                       (uint16_t)StaticBlockRegistry[End].Schema->Type == Type) End++;
                Word = EnumTypeWordOf(Type, End - Start - 1);
                Start = End;
            }
#ifdef USE_DYNAMIC_BLOCKS
            else if (DynamicBlockRegistry.BlockCount > 0) {
                uint16_t Highest = (uint16_t)(DynamicBlockRegistry.BlockCount - 1);
                uint16_t BankType = BlockTypeRange::DynamicType((uint16_t)(Highest >> BlockTypeRange::BankShift));
                Word = (uint16_t)((BankType << 8) | (Highest & 0xFF));
            }
#endif
            Dst[Len++] = (uint8_t)Word;
            Dst[Len++] = (uint8_t)(Word >> 8);
            Idx++;
        }
        SendFragFragment(Frame, Field, Frags, Len);
    }
}

inline void RegisterService::HandleEnumerateBlocks(const PacketFrame &Frame) {
    SendTypeWordsStream(Frame);
}

// CID 1: a block's Field&Key words. The request carries the packed `(type << 6) | instance`
// word (a uint16 on the wire). An absent or tombstoned block reports an empty list rather than a
// failure.
inline void RegisterService::HandleEnumerateFields(const PacketFrame &Frame) {
    if (PayloadBytes(Frame) != 4) { RespondStatus(Frame, false); return; }
    uint16_t packed = LoadUnaligned<uint16_t>(Frame.Payload);
    uint16_t Type = (packed >> 6) & 0x3FF;
    uint8_t Inst = (uint8_t)(packed & 0x3F);
    uint16_t Gi = 0; // the global index of a banked (script/dynamic) block; ctx points at it
    (void)Inst; (void)Gi; // unused when neither USE_SCRIPTS nor USE_DYNAMIC_BLOCKS is set
    EnumSrc Src = EnumSrc::System; // overridden below; unused for a zero-count reply
    const void *Ctx = nullptr;
    uint16_t Count = 0;

    if (Type == 0) { // the System block: virtual, its schema is the fixed System_Entries
        Src = EnumSrc::System;
        Count = SystemEntryCount;
#ifdef USE_SCRIPTS
    } else if (BlockTypeRange::IsScript(Type)) { // a loaded script: its inputs and outputs, per instance
        Gi = BlockTypeRange::ScriptGlobal(Type, Inst);
        LoadedScript *Loaded = Script.Active(Gi);
        if (Loaded) {
            Src = EnumSrc::Script;
            Ctx = &Gi;
            Count = (uint16_t)(Loaded->InCount + Loaded->OutCount);
        }
#endif
#ifdef USE_DYNAMIC_BLOCKS
    } else if (BlockTypeRange::IsDynamic(Type)) { // a dynamic block: its live entries, per instance
        Gi = BlockTypeRange::DynamicGlobal(Type, Inst);
        DynamicBlockDescriptor *Block = Gi < DynamicBlockRegistry.BlockCount
                                        ? DynamicBlockRegistry.GetBlock(Gi)
                                        : nullptr;
        if (Block && Block->Present) {
            Src = EnumSrc::Dynamic;
            Ctx = Block;
            Count = Block->EntryCount;
        }
#endif
    } else { // a static registry entry: the schema is the type's, every instance answers alike
        int Idx = FindStaticBlock(Type, 0);
        if (Idx >= 0) {
            Src = EnumSrc::Static;
            Ctx = StaticBlockRegistry[Idx].Schema;
            Count = StaticBlockRegistry[Idx].Schema->EntryCount;
        }
    }
    SendU16Stream(Frame, Count, Src, Ctx);
}
