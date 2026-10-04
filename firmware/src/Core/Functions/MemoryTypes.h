#pragma once

#include <cstdint>
#include <cstring>
#include <cstddef>
#include "Core/Types/Enums.h"

#define BLOCK_NAME_LEN 16

// Copies a block name into a fixed BLOCK_NAME_LEN field (Docs/Services/Register.md "Dynamic
// Block Table": Name is 16 chars with no terminator). A shorter name is space-padded.
static inline void SetBlockName(char *dst, const char *src, uint16_t len) {
    if (len > BLOCK_NAME_LEN) len = BLOCK_NAME_LEN;
    if (len && src) memcpy(dst, src, len);
    if (len < BLOCK_NAME_LEN) memset(dst + len, ' ', (size_t)(BLOCK_NAME_LEN - len));
}

// Combines a field index and a key into the 16-bit Field&Key sort key.
constexpr uint16_t MakeFieldKey(uint8_t field, uint8_t key) { return (uint16_t)(((uint16_t)field << 8) | key); }
constexpr uint8_t FieldOf(uint16_t fieldKey) { return (uint8_t)(fieldKey >> 8); }
constexpr uint8_t KeyOf(uint16_t fieldKey) { return (uint8_t)fieldKey; }

// The descriptor (Docs/Services/Register.md "Map entry"): Type(16) | Size(8) | Flags(8).
// One type, internal and on the wire. The key is not part of it - it travels in the request's /
// reply's BlockInfo, or in a table entry's Field&Key.
struct ValueInfo
{
    uint16_t Type;
    uint8_t Size;  // value length in bytes
    uint8_t Flags;
};

// The passive flags, in the doc's table order.
enum ValueFlags : uint8_t
{
    ValueReadOnly   = 0x01,
    ValuePersistent = 0x02,
    ValueTrigger    = 0x04,
};

inline uint16_t ValueInfoType(const ValueInfo &v) { return (uint16_t)(v.Type & 0x3FF); }
inline uint16_t ValueInfoType(uint16_t type) { return (uint16_t)(type & 0x3FF); }
inline bool ValueIsReadOnly(const ValueInfo &v)   { return (v.Flags & ValueReadOnly) != 0; }
inline bool ValueIsPersistent(const ValueInfo &v) { return (v.Flags & ValuePersistent) != 0; }

struct FieldResult
{
    ValueInfo Descriptor = { (uint16_t)DataType::None, 0, 0 };
    void *Data = nullptr;
};

struct StaticBlockDescriptor;

typedef bool (*FieldTrigger)(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);

// 1. The Schema (All members are const)
//
// Docs/Services/Register.md "Static Block Type Table": a literal table of entries
// (Field&Key + MemoryOffset + ValueInfo) plus a literal trigger table (Field&Key + function
// pointer) that holds only the fields that actually have a trigger.
struct BlockEntry
{
    uint16_t FieldKey; // (field << 8) | key
    uint16_t Offset;   // byte offset within the value space the field's Persistent flag selects
    ValueInfo Info;
};

struct BlockTrigger
{
    uint16_t FieldKey;
    FieldTrigger Fn;
};

struct BlockSchema
{
    const BlockEntry *const Entries;
    const uint16_t EntryCount;
    const BlockTrigger *const Triggers; // nullptr when the block has no triggers
    const uint16_t TriggerCount;
    const BlockType Type;
};

struct StaticBlockDescriptor
{
    // Pointers into the board's two flat spaces (Docs/Services/Register.md "System + Static
    // memory blocks"): the persistent space (settings, mirrored 1:1 to .SV) and the volatile
    // space. An entry's offset is a byte offset from whichever pointer the field's
    // Persistent flag selects.
    void* const VolatileData;
    void* const PersistentData;
    const BlockSchema* const Schema;
    const char *const Name;
    // Computed-field getter (the System block): returns the field's value pointer, or nullptr
    // to fall back to the storage path (so the persistent Name/NetID keep their real storage).
    void* (*const VirtualGet)(uint16_t field, uint8_t key) = nullptr;

    const void* Base(const ValueInfo &info) const {
        return ValueIsPersistent(info) ? PersistentData : VolatileData;
    }

    const BlockEntry* FindEntry(uint16_t field, uint8_t key) const {
        // Static single-key fields are addressed with key 0xFF by the app; normalise it to the
        // entries' key 0. Multi-key blocks (e.g. the System block) match their exact key.
        const uint16_t fk = MakeFieldKey(field, key == 0xFF ? 0 : key);
        for (uint16_t i = 0; i < Schema->EntryCount; i++)
            if (Schema->Entries[i].FieldKey == fk) return &Schema->Entries[i];
        return nullptr;
    }

    FieldTrigger FindTrigger(uint16_t field, uint8_t key) const {
        const uint16_t fk = MakeFieldKey(field, key == 0xFF ? 0 : key);
        for (uint16_t i = 0; i < Schema->TriggerCount; i++)
            if (Schema->Triggers[i].FieldKey == fk) return Schema->Triggers[i].Fn;
        return nullptr;
    }

    // Unified entry retrieval by (field, key) (linear over the literal table).
    FieldResult Get(uint16_t field, uint8_t key) const {
        const BlockEntry *e = FindEntry(field, key);
        if (!e) return FieldResult{};
        FieldResult Output;
        Output.Descriptor = e->Info;
        if (VirtualGet) {
            void *p = VirtualGet(field, key);
            if (p) { Output.Data = p; return Output; }
        }
        Output.Data = const_cast<uint8_t*>(static_cast<const uint8_t*>(Base(e->Info))) + e->Offset;
        return Output;
    }

    // Unified setter interface. `desc` is the request's ValueInfo (type + flags).
    bool Set(uint16_t field, uint8_t key, const void* Input, uint16_t Length, const ValueInfo &desc) const {
        FieldResult Field = Get(field, key);

        if (!Field.Data)
            return false;

        if (ValueIsReadOnly(Field.Descriptor))
            return false;

        if (ValueInfoType(Field.Descriptor) != ValueInfoType(desc))
            return false;

        uint8_t pad_buf[32];
        const void *data = Input;
        uint16_t data_len = Length;
        uint16_t field_type = ValueInfoType(Field.Descriptor);
        // String/Filename fields are space-padded up to their declared size when a shorter
        // value is written, and clamped to it when a longer one is (filenames are fixed
        // 8-char records; the System Name is a fixed 16).
        if (field_type == (uint16_t)DataType::String || field_type == (uint16_t)DataType::Filename)
        {
            if (Length < Field.Descriptor.Size && Field.Descriptor.Size <= sizeof(pad_buf))
            {
                memset(pad_buf, ' ', sizeof(pad_buf));
                memcpy(pad_buf, Input, Length);
                data = pad_buf;
                data_len = Field.Descriptor.Size;
            }
            else if (Length > Field.Descriptor.Size)
            {
                data_len = Field.Descriptor.Size; // clamp; memcpy copies Size bytes anyway
            }
        }
        else if (Length != Field.Descriptor.Size)
        {
            return false;
        }

        const FieldTrigger fn = FindTrigger(field, key);
        if (fn != nullptr)
            return fn(*this, field, data, data_len);

        memcpy(Field.Data, data, Field.Descriptor.Size);
        return true;
    }
};

extern const StaticBlockDescriptor static_block_registry[];
extern const size_t static_block_num;
