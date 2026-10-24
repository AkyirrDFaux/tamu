#pragma once

#include <cstdint>
#include <cstring>
#include <cstddef>
#include "Core/Types/Enums.h"

#define BLOCK_NAME_LEN 16

// Copies a block name into a fixed BLOCK_NAME_LEN field (Docs/Services/Register.md "Dynamic
// Block Table": Name is 16 chars). A shorter name is null-padded (B10).
static inline void SetBlockName(char *Dst, const char *Src, uint16_t Len) {
    if (Len > BLOCK_NAME_LEN) Len = BLOCK_NAME_LEN;
    if (Len && Src) memcpy(Dst, Src, Len);
    if (Len < BLOCK_NAME_LEN) memset(Dst + Len, 0x00, (size_t)(BLOCK_NAME_LEN - Len));
}

// Combines a field index and a key into the 16-bit Field&Key sort key.
constexpr uint16_t MakeFieldKey(uint8_t Field, uint8_t Key) { return (uint16_t)(((uint16_t)Field << 8) | Key); }
constexpr uint8_t FieldOf(uint16_t FieldKey) { return (uint8_t)(FieldKey >> 8); }

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

inline uint16_t ValueInfoType(const ValueInfo &Info) { return (uint16_t)(Info.Type & 0x3FF); }
inline uint16_t ValueInfoType(uint16_t Type) { return (uint16_t)(Type & 0x3FF); }
inline bool ValueIsReadOnly(const ValueInfo &Info)   { return (Info.Flags & ValueReadOnly) != 0; }
inline bool ValueIsPersistent(const ValueInfo &Info) { return (Info.Flags & ValuePersistent) != 0; }

// Whether a value of `Len` bytes may be applied to a target at all.
//
// A zero-length value is not a value. Storing one sets the entry's Size to 0, and a size-0 entry
// resolves to no data (GetKey/DynamicBlockDescriptor hands back DataPtr = nullptr), so every later
// update to that field is dropped as unresolvable and the target stays dead until a write that
// carries a size again comes along. Measured on the rig (bambi Dynamic.0 field 6): one register
// write carrying Size 0 took its meta from 04000100 to 04000000, and a real 1-byte value pushed
// afterwards on the subscription row renewed the row's lease while the field never changed. Every
// value write is therefore gated on this: refuse a 0-length value and leave the target as it was.
static inline bool ValueUpdateApplicable(uint8_t Len) { return Len != 0; }

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
    void* (*const VirtualGet)(uint16_t Field, uint8_t Key) = nullptr;

    const void* Base(const ValueInfo &Info) const {
        return ValueIsPersistent(Info) ? PersistentData : VolatileData;
    }

    const BlockEntry* FindEntry(uint16_t Field, uint8_t Key) const {
        // Static single-key fields are addressed with key 0xFF by the app; normalise it to the
        // entries' key 0. Multi-key blocks (e.g. the System block) match their exact key.
        const uint16_t Fk = MakeFieldKey(Field, Key == 0xFF ? 0 : Key);
        for (uint16_t i = 0; i < Schema->EntryCount; i++)
            if (Schema->Entries[i].FieldKey == Fk) return &Schema->Entries[i];
        return nullptr;
    }

    FieldTrigger FindTrigger(uint16_t Field, uint8_t Key) const {
        const uint16_t Fk = MakeFieldKey(Field, Key == 0xFF ? 0 : Key);
        for (uint16_t i = 0; i < Schema->TriggerCount; i++)
            if (Schema->Triggers[i].FieldKey == Fk) return Schema->Triggers[i].Fn;
        return nullptr;
    }

    // Unified entry retrieval by (field, key) (linear over the literal table).
    FieldResult Get(uint16_t Field, uint8_t Key) const {
        const BlockEntry *Entry = FindEntry(Field, Key);
        if (!Entry) return FieldResult{};
        FieldResult Output;
        Output.Descriptor = Entry->Info;
        if (VirtualGet) {
            void *Ptr = VirtualGet(Field, Key);
            if (Ptr) { Output.Data = Ptr; return Output; }
        }
        Output.Data = const_cast<uint8_t*>(static_cast<const uint8_t*>(Base(Entry->Info))) + Entry->Offset;
        return Output;
    }

    // Unified setter interface. `desc` is the request's ValueInfo (type + flags).
    bool Set(uint16_t Field, uint8_t Key, const void* Input, uint16_t Length, const ValueInfo &Desc) const {
        FieldResult Result = Get(Field, Key);

        if (!Result.Data)
            return false;

        if (ValueIsReadOnly(Result.Descriptor))
            return false;

        if (ValueInfoType(Result.Descriptor) != ValueInfoType(Desc))
            return false;

        uint8_t PadBuf[32];
        const void *Data = Input;
        uint16_t DataLen = Length;
        uint16_t FieldType = ValueInfoType(Result.Descriptor);
        // String/Filename fields are null-padded up to their declared size when a shorter
        // value is written, and clamped to it when a longer one is (filenames are fixed
        // 8-char records; the System Name is a fixed 16).
        // Text/Name/Filename are fixed-size character fields (Docs/Services/Register.md).
        if (FieldType == (uint16_t)DataType::Text || FieldType == (uint16_t)DataType::Name ||
            FieldType == (uint16_t)DataType::Filename)
        {
            if (Length < Result.Descriptor.Size)
            {
                // A value larger than the local staging buffer cannot be null-padded without
                // reading past the caller's `Length`-byte source; reject it instead.
                if (Result.Descriptor.Size > sizeof(PadBuf))
                    return false;
                memset(PadBuf, 0x00, sizeof(PadBuf));
                memcpy(PadBuf, Input, Length);
                Data = PadBuf;
                DataLen = Result.Descriptor.Size;
            }
            else if (Length > Result.Descriptor.Size)
            {
                DataLen = Result.Descriptor.Size; // clamp; memcpy copies Size bytes anyway
            }
        }
        else if (Length != Result.Descriptor.Size)
        {
            return false;
        }

        const FieldTrigger Fn = FindTrigger(Field, Key);
        if (Fn != nullptr)
            return Fn(*this, Field, Data, DataLen);

        memcpy(Result.Data, Data, Result.Descriptor.Size);
        return true;
    }
};

extern const StaticBlockDescriptor StaticBlockRegistry[];
extern const size_t StaticBlockNum;
