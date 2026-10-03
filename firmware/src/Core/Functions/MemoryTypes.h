#pragma once

#include <cstdint>
#include <cstring>
#include <cstddef>
#include "Core/Types/Enums.h"

#define BLOCK_NAME_LEN 16

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
inline bool ValueIsTrigger(const ValueInfo &v)    { return (v.Flags & ValueTrigger) != 0; }

struct FieldResult
{
    ValueInfo Descriptor = { (uint16_t)DataType::Unknown, 0, 0 };
    void *Data = nullptr;
};

struct StaticBlockDescriptor;

typedef bool (*FieldTrigger)(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);

// 1. The Schema (All members are const)
struct BlockSchema
{
    const ValueInfo *const Map;
    const FieldTrigger *const Triggers; // Indexed by field number, nullptr = no trigger
    const uint16_t *const Offsets;      // Precomputed byte offsets within the block's RAM struct
    const BlockType Type;
    const uint16_t MapCount;
    const uint16_t VolatileSize;   // bytes this block's volatile half occupies
    const uint16_t PersistentSize; // bytes this block's persistent half occupies
};

struct StaticBlockDescriptor
{
    // Pointers into the board's two flat spaces (Docs/Services/Register.md "System + Static
    // memory blocks"): the persistent space (settings, mirrored 1:1 to .SV) and the volatile
    // space. The schema's offsets are byte offsets from whichever pointer the field's
    // Persistent flag selects.
    void* const VolatileData;
    void* const PersistentData;
    const BlockSchema* const Schema;
    const char *const Name;

    const void* Base(const ValueInfo &info) const {
        return ValueIsPersistent(info) ? PersistentData : VolatileData;
    }

    // Unified entry retrieval — O(1) via precomputed offset
    FieldResult Get(uint16_t Index) const {
        FieldResult Output;
        if (Index >= Schema->MapCount) return Output;

        const ValueInfo &info = Schema->Map[Index];
        Output.Descriptor = info;
        Output.Data = const_cast<uint8_t*>(static_cast<const uint8_t*>(Base(info))) + Schema->Offsets[Index];
        return Output;
    }

    // Unified setter interface. `desc` is the request's ValueInfo (type + flags).
    bool Set(uint16_t Index, const void* Input, uint16_t Length, const ValueInfo &desc) const {
        FieldResult Field = Get(Index);

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
        // value is written (filenames are fixed 8-char records).
        if ((field_type == (uint16_t)DataType::String || field_type == (uint16_t)DataType::Filename) &&
            Length < Field.Descriptor.Size && Field.Descriptor.Size <= sizeof(pad_buf))
        {
            memset(pad_buf, ' ', sizeof(pad_buf));
            memcpy(pad_buf, Input, Length);
            data = pad_buf;
            data_len = Field.Descriptor.Size;
        }
        else if (Length != Field.Descriptor.Size)
        {
            return false;
        }

        // Indexed trigger lookup — O(1), nullptr = no trigger
        if (Schema->Triggers != nullptr && Schema->Triggers[Index] != nullptr) {
            return Schema->Triggers[Index](*this, Index, data, data_len);
        }

        memcpy(Field.Data, data, Field.Descriptor.Size);
        return true;
    }
};

extern const StaticBlockDescriptor static_block_registry[];
extern const size_t static_block_num;
