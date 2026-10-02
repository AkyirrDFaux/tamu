#pragma once

// CID 0: enumerate blocks, fields and keys.
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

// The System block's field/key list, flattened as (field << 8 | key) and matching
// RegisterGetSystemField's cases. A flat table beats re-deriving it from per-field key counts:
// the unrolled double loop cost more flash than these 34 bytes.
static const uint16_t kSystemFields[] = {
    0x0000, 0x0001, 0x0002,                 // field 0: three keys
    0x0100, 0x0200,                         // fields 1, 2: one key each
    0x0300, 0x0301, 0x0302, 0x0303, 0x0304, // field 3: five keys
    0x0400, 0x0401,                         // field 4: two keys
    0x0500, 0x0501,                         // field 5: two keys
    0x0600, 0x0700, 0x0800,                 // fields 6, 7, 8: one key each
};
static const uint16_t kSystemFieldCount = sizeof(kSystemFields) / sizeof(kSystemFields[0]);

// Which list is being streamed.
enum class EnumSrc : uint8_t { Types, System, Static, Dynamic, Script };

// The block-type list word at index `i`. The System block is always first: its word is the only
// zero and it is first, which is what lets the app tell a real zero from the wire's trailing
// padding. The static registry is grouped by type (see the board Main.h - STATLOG's index depends
// on it), so a run of equal types is one list entry with `run - 1` as its highest instance.
static uint16_t EnumTypeWord(uint16_t i) {
    if (i == 0) return 0; // the System block: type 0, instance 0
    uint16_t seen = 1;
    for (size_t start = 0; start < static_block_num;) {
        uint16_t t = (uint16_t)static_block_registry[start].Schema->Type;
        size_t end = start + 1;
        while (end < static_block_num &&
               (uint16_t)static_block_registry[end].Schema->Type == t) end++;
        if (seen++ == i) return (uint16_t)((t << 6) | ((end - start - 1) & 0x3F));
        start = end;
    }
#ifndef DISABLE_DYNAMIC_MEMORY
    if (seen == i && dynamic_block_registry.block_count > 0)
        return (uint16_t)(((uint16_t)BlockType::Dynamic << 6) |
                          ((dynamic_block_registry.block_count - 1) & 0x3F));
#endif
    return 0;
}

static uint16_t EnumTypeCount() {
    uint16_t count = 1; // the System block
    for (size_t i = 0; i < static_block_num; i++)
        if (i == 0 || static_block_registry[i].Schema->Type != static_block_registry[i - 1].Schema->Type)
            count++;
#ifndef DISABLE_DYNAMIC_MEMORY
    if (dynamic_block_registry.block_count > 0) count++;
#endif
    return count;
}

#ifdef USE_SCRIPTS
static uint16_t EnumScriptWord(uint8_t inst, uint16_t i) {
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
    case EnumSrc::Types:
        return EnumTypeWord(i);
    case EnumSrc::System:
        return kSystemFields[i];
    case EnumSrc::Static: {
        const BlockSchema *sc = (const BlockSchema *)ctx;
        return (uint16_t)((i << 8) | sc->Map[i].Key);
    }
#ifndef DISABLE_DYNAMIC_MEMORY
    case EnumSrc::Dynamic: {
        const DynamicBlockDescriptor *b = (const DynamicBlockDescriptor *)ctx;
        return b->table[i].fieldKey;
    }
#endif
#ifdef USE_SCRIPTS
    case EnumSrc::Script:
        return EnumScriptWord(*(const uint8_t *)ctx, i);
#endif
    }
    return 0;
}

// Streams `count` words as FRAG fragments, one lookup at a time - no buffer, the content goes
// straight into the frame payload the send path already has. `count` may be 0 (an empty reply).
static void SendU16Stream(const PacketFrame &frame, uint16_t count, EnumSrc src, const void *ctx) {
    const uint16_t kWordsPerFrag = 32; // 64-byte fragments, two bytes per word
    uint16_t frags = (uint16_t)((count + kWordsPerFrag - 1) / kWordsPerFrag);
    if (frags == 0) frags = 1;

    PacketFrame tx;
    uint16_t idx = 0; // the next word to emit
    for (uint16_t f = 0; f < frags; f++) {
        uint8_t *dst = tx.payload + 4;
        uint16_t n = 0;
        while (n < kWordsPerFrag * 2 && idx < count) {
            uint16_t w = EnumWord(src, ctx, idx++);
            dst[n++] = (uint8_t)w;
            dst[n++] = (uint8_t)(w >> 8);
        }
        uint8_t flags = FLAG_TYPE | FLAG_FRAG;
        if (f == 0) flags |= FLAG_START;
        if (f == frags - 1) flags |= FLAG_STOP;
        WriteFragInfo(tx.payload, f, frags);
        FinalizeReply(tx, frame, flags, (uint16_t)(4 + n));
        DispatchPacket(tx);
    }
}

// CID 0's dispatcher: the request's own shape says which enumeration is wanted. The wire pads
// every payload to 4 bytes, so an empty request is the type list and a 4-byte one is the packed
// `(type << 6) | instance` block request (a uint16 on the wire). An absent or tombstoned block
// reports an empty list rather than a failure.
static void HandleEnumerate(const PacketFrame &frame) {
    uint16_t bytes = PayloadBytes(frame);
    EnumSrc src = EnumSrc::Types;
    const void *ctx = nullptr;
    uint16_t count = 0;

    if (bytes == 0) {
        count = EnumTypeCount();
    } else if (bytes == 4) {
        uint16_t packed = (uint16_t)(frame.payload[0] | (frame.payload[1] << 8));
        uint16_t type = (packed >> 6) & 0x3FF;
        uint8_t inst = (uint8_t)(packed & 0x3F);
        if (type == 0) { // the System block: virtual, its field/key list is fixed
            src = EnumSrc::System;
            count = kSystemFieldCount;
#ifdef USE_SCRIPTS
        } else if (type == 0x3FE) { // a loaded script: its inputs and outputs, per instance
            LoadedScript *s = ScriptActive(inst);
            if (s) {
                src = EnumSrc::Script;
                ctx = &inst;
                count = (uint16_t)(s->inCount + s->outCount);
            }
#endif
#ifndef DISABLE_DYNAMIC_MEMORY
        } else if (type == 0x3FF) { // a dynamic block: its live entries, per instance
            DynamicBlockDescriptor *b = inst < dynamic_block_registry.block_count
                                            ? dynamic_block_registry.GetBlock(inst)
                                            : nullptr;
            if (b && b->type != BlockType::None) {
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
                count = static_block_registry[idx].Schema->MapCount;
            }
        }
    } else {
        RespondStatus(frame, false);
        return;
    }
    SendU16Stream(frame, count, src, ctx);
}
