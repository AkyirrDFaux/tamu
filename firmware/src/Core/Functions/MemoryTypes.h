#pragma once

#include <cstdint>
#include <cstring>
#include <cstddef>
#include "Core/Types/Enums.h"

#define BLOCK_NAME_LEN 24

// BlockMeta (Data Formats.md): Flags 6bit | Type 10bit | (Padding OR Key) | Length of Value
struct BlockMeta
{
    uint16_t FlagsAndType; // 6bit flags | 10bit type
    uint8_t Key;           // Padding (or key for keyed entries)
    uint8_t Size;          // Length of value in bytes
};

inline uint16_t BlockMetaFlags(uint16_t v)
{
    return v & BLOCK_META_FLAGS_MASK;
}

inline uint16_t BlockMetaType(uint16_t v)
{
    return v & BLOCK_META_TYPE_MASK;
}

struct FieldResult
{
    BlockMeta Descriptor = {DataType::Unknown | FieldFlags::None, 0x00, 0};
    void *Data = nullptr;
};

struct StaticBlockDescriptor;

typedef bool (*FieldTrigger)(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);

// 1. The Schema (All members are const)
struct BlockSchema
{
    const BlockMeta *const Map;
    const FieldTrigger *const Triggers; // Indexed by field number, nullptr = no trigger
    const uint16_t *const Offsets;      // Precomputed byte offsets within the block's RAM struct
    const BlockType Type;
    const uint16_t MapCount;
};

struct StaticBlockDescriptor
{
    void* const Data;
    const BlockSchema* const Schema;
    const char *const Name;

    // Unified entry retrieval — O(1) via precomputed offset
    FieldResult Get(uint16_t Index) const {
        FieldResult Output;
        if (Index >= Schema->MapCount) return Output;

        uint8_t* current_ptr = static_cast<uint8_t*>(Data) + Schema->Offsets[Index];
        Output.Descriptor = Schema->Map[Index];
        Output.Data = static_cast<void*>(current_ptr);
        return Output;
    }

    // Unified setter interface
    bool Set(uint16_t Index, const void* Input, uint16_t Length, uint16_t InputTypeAndFlag) const {
        FieldResult Field = Get(Index);

        if (!Field.Data)
            return false;

        if (Field.Descriptor.FlagsAndType & FieldFlags::ReadOnly)
            return false;

        if (BlockMetaType(Field.Descriptor.FlagsAndType) != BlockMetaType(InputTypeAndFlag))
            return false;

        uint8_t pad_buf[32];
        const void *data = Input;
        uint16_t data_len = Length;
        if (BlockMetaType(Field.Descriptor.FlagsAndType) == (uint16_t)DataType::String &&
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
