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

// Extracts the 6-bit flags field from a BlockMeta FlagsAndType word
inline uint16_t BlockMetaFlags(uint16_t v)
{
    return v & BLOCK_META_FLAGS_MASK;
}

// Extracts the 10-bit type field from a BlockMeta FlagsAndType word
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

struct TriggerEntry
{
    const FieldTrigger Trigger;
    const uint16_t Index;
};

// 1. The Schema (All members are const)
struct BlockSchema
{
    const BlockMeta *const Map;
    const TriggerEntry *const Triggers;
    const BlockType Type;
    const uint16_t MapCount;
    const uint16_t TriggerCount;
};

struct StaticBlockDescriptor
{
    void* const Data;          // Immutable RAM/Flash pointer
    const BlockSchema* const Schema;
    const char *const Name;    // Block name (fixed string)

    // --- Member Functions ---

    // Unified entry retrieval
    FieldResult Get(uint16_t Index) const {
        FieldResult Output;
        if (Index >= Schema->MapCount) return Output;

        // Fields are laid out 4-byte aligned (the same alignment the dynamic/keyed
        // descriptors use), so a static block with a narrow field (e.g. a 1-byte Enum)
        // still addresses the following fields at their real struct offsets.
        size_t offset = 0;
        for (uint16_t i = 0; i < Index; ++i)
            offset += ((size_t)Schema->Map[i].Size + 3) & ~(size_t)3;

        uint8_t* current_ptr = static_cast<uint8_t*>(Data) + offset;
        Output.Descriptor = Schema->Map[Index];
        Output.Data = static_cast<void*>(current_ptr);
        return Output;
    }

    // Unified setter interface
    bool Set(uint16_t Index, const void* Input, uint16_t Length, uint16_t InputTypeAndFlag) const {
        FieldResult Field = Get(Index);

        // 1. Basic structural checks
        if (!Field.Data)
            return false;

        // 2. Security: Read-Only check
        if (Field.Descriptor.FlagsAndType & FieldFlags::ReadOnly)
            return false;

        // 3. Type Validation
        if (BlockMetaType(Field.Descriptor.FlagsAndType) != BlockMetaType(InputTypeAndFlag))
            return false;

        // 4. String fields accept SHORTER input, space-padded to the field size
        //    (the CLI/app can write "SNAKE" to an 8-byte layout name). The pad
        //    buffer is bounded so this stays small on the DAS's tiny stack.
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

        // 5. Trigger/Validator Gatekeeper (sees the padded length)
        if (Schema->Triggers != nullptr) {
            for (uint16_t i = 0; i < Schema->TriggerCount; ++i) {
                if (Schema->Triggers[i].Index == Index) {
                    return Schema->Triggers[i].Trigger(*this, Index, data, data_len);
                }
            }
        }

        // 6. Final commit
        memcpy(Field.Data, data, Field.Descriptor.Size);
        return true;
    }
};

// Compiled-in (System Memory) block registry, defined per device (e.g. Devices/<device>/Main.h)
extern const StaticBlockDescriptor static_block_registry[];
extern const size_t static_block_num;

