#pragma once

#ifdef USE_DYNAMIC_MEMORY

#include <cstring>
#include <cstdlib>
#include "Core/Functions/Memory.h"

// Dynamic Memory block registry (runtime allocated).
DynamicRegistry dynamic_block_registry;

// Encoded Storage file name holding this service's backup registry.
static const char *DynamicBackupName()
{
    static constexpr char name[8] = {'D', 'Y', 'N', 'M', 'E', 'M', ' ', ' '};
    return name;
}

// Creates a new dynamic block of `type` named by `name` (name_len bytes). Returns the block or nullptr.
static DynamicBlockDescriptor *CreateDynamicBlock(BlockType type, const uint8_t *name, uint16_t name_len)
{
    if (!dynamic_block_registry.AddBlock(type))
        return nullptr;
    DynamicBlockDescriptor &block = *dynamic_block_registry.GetBlock(dynamic_block_registry.block_count - 1);
    uint16_t len = name_len;
    if (len > BLOCK_NAME_LEN - 1) len = BLOCK_NAME_LEN - 1;
    if (len && name) memcpy(block.Name, name, len);
    block.Name[len] = '\0';
    return &block;
}

// Sends a Create response: BlockIndex + BlockMeta + value (the block name).
static void RespondCreate(const PacketFrame &frame, uint16_t new_block, const BlockMeta &desc,
                          const uint8_t *value, uint16_t value_len)
{
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint16_t cursor = 0;
    BlockIndex out_index = {(uint8_t)new_block, INVALID_INDEX, INVALID_INDEX};
    memcpy(payload + cursor, &out_index, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
    BlockMeta desc_copy = desc;
    uint16_t name_len = value_len;
    if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;
    desc_copy.Size = (uint8_t)name_len;
    memcpy(payload + cursor, &desc_copy, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
    if (name_len) { memcpy(payload + cursor, value, name_len); cursor += name_len; }
    SendResponse(frame, payload, (uint8_t)cursor);
}

// Sends an echo response (BlockIndex + BlockMeta + value).
static void RespondEcho(const PacketFrame &frame, const BlockIndex *idx, const BlockMeta &desc,
                        const uint8_t *value, uint16_t value_len)
{
    uint8_t payload[MAX_PAYLOAD_SIZE];
    uint16_t cursor = 0;
    memcpy(payload + cursor, idx, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
    BlockMeta desc_copy = desc;
    desc_copy.Size = (uint8_t)value_len;
    memcpy(payload + cursor, &desc_copy, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
    uint16_t len = value_len;
    if (len > MAX_PAYLOAD_SIZE - cursor) len = MAX_PAYLOAD_SIZE - cursor;
    if (len) { memcpy(payload + cursor, value, len); cursor += len; }
    SendResponse(frame, payload, (uint8_t)cursor);
}

// Serialises the whole registry into `out`; returns the byte count (0 on overflow).
// Blocks flagged as Deleted are skipped: they are deallocated on the next Save.
template <typename T>
static uint16_t SerializeRegistry(BlockRegistry<T> &registry, uint8_t *out, uint16_t cap)
{
    uint16_t cursor = 0;
    uint16_t block_count = 0;
    for (uint16_t index = 0; index < registry.block_count; index++)
        if (registry.GetBlock(index)->type != BlockType::Deleted)
            block_count++;
    if (cursor + 2 > cap) return 0;
    memcpy(out + cursor, &block_count, 2); cursor += 2;

    for (uint16_t index = 0; index < registry.block_count; index++)
    {
        T &block = *registry.GetBlock(index);
        if (block.type == BlockType::None || block.type == BlockType::Deleted)
            continue;
        uint8_t name_length = (uint8_t)strlen(block.Name);
        if (cursor + 1 + name_length > cap) return 0;
        out[cursor++] = name_length;
        if (name_length) { memcpy(out + cursor, block.Name, name_length); cursor += name_length; }

        uint16_t type = (uint16_t)block.type;
        if (cursor + 2 > cap) return 0;
        memcpy(out + cursor, &type, 2); cursor += 2;

        if (cursor + 2 > cap) return 0;
        memcpy(out + cursor, &block.map_count, 2); cursor += 2;

        uint16_t map_bytes = block.map_count * sizeof(BlockMeta);
        if (cursor + map_bytes > cap) return 0;
        if (map_bytes) { memcpy(out + cursor, block.map, map_bytes); cursor += map_bytes; }

        if (cursor + 2 > cap) return 0;
        memcpy(out + cursor, &block.length, 2); cursor += 2;
        if (cursor + block.length > cap) return 0;
        if (block.length) { memcpy(out + cursor, block.data_ptr, block.length); cursor += block.length; }
    }
    return cursor;
}

// Rebuilds the registry from a serialised buffer, replacing any existing blocks.
template <typename T>
static bool DeserializeRegistry(BlockRegistry<T> &registry, const uint8_t *in, uint16_t len)
{
    uint16_t cursor = 0;
    if (len < 2) return false;
    uint16_t block_count;
    memcpy(&block_count, in + cursor, 2); cursor += 2;

    // Free any existing blocks first.
    while (registry.block_count > 0)
        registry.RemoveBlock(registry.block_count - 1);

    for (uint16_t index = 0; index < block_count; index++)
    {
        if (cursor + 1 > len) return false;
        uint8_t name_length = in[cursor++];
        if (cursor + name_length > len) return false;
        const uint8_t *name = in + cursor; cursor += name_length;

        if (cursor + 2 > len) return false;
        uint16_t type; memcpy(&type, in + cursor, 2); cursor += 2;

        if (cursor + 2 > len) return false;
        uint16_t map_count; memcpy(&map_count, in + cursor, 2); cursor += 2;

        uint16_t map_bytes = (uint16_t)map_count * sizeof(BlockMeta);
        if (cursor + map_bytes > len) return false;
        const BlockMeta *map = (const BlockMeta *)(in + cursor); cursor += map_bytes;

        if (cursor + 2 > len) return false;
        uint16_t data_length; memcpy(&data_length, in + cursor, 2); cursor += 2;
        if (cursor + data_length > len) return false;
        const uint8_t *data = in + cursor; cursor += data_length;

        if (!registry.AddBlock((BlockType)type))
            return false;
        T &block = *registry.GetBlock(registry.block_count - 1);

        uint16_t nn = name_length;
        if (nn > BLOCK_NAME_LEN - 1) nn = BLOCK_NAME_LEN - 1;
        memcpy(block.Name, name, nn); block.Name[nn] = '\0';

        if (map_count)
        {
            block.map = (BlockMeta *)malloc(map_bytes);
            if (!block.map) { registry.RemoveBlock(registry.block_count - 1); return false; }
            memcpy(block.map, map, map_bytes);
            block.map_allocated = block.map_count = map_count;
        }
        if (data_length)
        {
            block.data_ptr = malloc(data_length);
            if (!block.data_ptr) { registry.RemoveBlock(registry.block_count - 1); return false; }
            memcpy(block.data_ptr, data, data_length);
            block.allocated = block.length = data_length;
        }
    }
    return true;
}

// Frees the registry array and every block's own allocations (cleanup helper for temporary
// registries used by per-block Save/Recall). Called only on throwaway registries.
template <typename T>
static void FreeRegistry(BlockRegistry<T> &registry)
{
    while (registry.block_count > 0)
        registry.RemoveBlock(registry.block_count - 1);
    free(registry.blocks); // the descriptor array allocated by add_block/realloc
    registry.blocks = nullptr;
    registry.block_count = registry.block_allocated = 0;
}

// Physically removes all Deleted-flagged blocks from the registry (after a successful Save).
template <typename T>
static void PurgeDeleted(BlockRegistry<T> &registry)
{
    for (uint16_t i = 0; i < registry.block_count; )
    {
        if (registry.GetBlock(i)->type == BlockType::Deleted ||
            registry.GetBlock(i)->type == BlockType::None)
            registry.RemoveBlock(i);
        else
            i++;
    }
}

// Deep-copies `source` into `destination` at position `index`, shifting existing blocks right.
// On allocation failure the partially-built block is rolled back so the registry is unchanged.
template <typename T>
static bool CopyBlockInto(BlockRegistry<T> &destination, uint16_t index, const DynamicBlockDescriptor &source)
{
    if (index > destination.block_count)
        return false;
    if (!destination.AddBlock(source.type))
        return false;
    const uint16_t added_at = destination.block_count - 1;
    for (uint16_t i = added_at; i > index; i--)
        destination.blocks[i] = destination.blocks[i - 1];

    T &new_block = *destination.GetBlock(index);
    // Zero the slot unconditionally: after the shift it may still hold aliased or stale
    // pointers from the moved neighbour (or a previously released block), which would be
    // double-freed later if the source block has no map/data of its own.
    new_block.data_ptr = nullptr;
    new_block.map = nullptr;
    new_block.length = 0;
    new_block.allocated = 0;
    new_block.map_count = 0;
    new_block.map_allocated = 0;
    new_block.type = source.type;
    uint16_t name_len = (uint16_t)strlen(source.Name);
    if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;
    memcpy(new_block.Name, source.Name, name_len); new_block.Name[name_len] = '\0';

    if (source.map_count)
    {
        new_block.map = (BlockMeta *)malloc(source.map_count * sizeof(BlockMeta));
        if (!new_block.map) { destination.RemoveBlock(index); return false; }
        memcpy(new_block.map, source.map, source.map_count * sizeof(BlockMeta));
        new_block.map_count = new_block.map_allocated = source.map_count;
    }
    if (source.length)
    {
        new_block.data_ptr = malloc(source.length);
        if (!new_block.data_ptr) { destination.RemoveBlock(index); return false; }
        memcpy(new_block.data_ptr, source.data_ptr, source.length);
        new_block.length = new_block.allocated = source.length;
    }
    return true;
}

// Saves a single block (or the whole registry when `block` is invalid) to the backup file,
// leaving the other stored blocks untouched. Deleted blocks are dropped by SerializeRegistry.
template <typename T>
static bool SaveRegistryBlock(BlockRegistry<T> &registry, uint8_t block, const char *fname)
{
    if (block == INVALID_BLOCK)
    {
        uint8_t buf[MEMORY_BACKUP_CAP];
        uint16_t count = SerializeRegistry(registry, buf, sizeof(buf));
        return count > 0 && WriteBackupFile(fname, buf, count);
    }
    if (block >= registry.block_count)
        return false;

    uint8_t buf[MEMORY_BACKUP_CAP];
    BlockRegistry<T> temp_registry;
    uint16_t count = ReadBackupFile(fname, buf, sizeof(buf));
    if (count == 0)
    {
        // No backup file yet: store the whole registry so the stored block indices stay
        // aligned with the live ones (a lone block N saved at index 0 would desync every
        // later per-block save).
        count = SerializeRegistry(registry, buf, sizeof(buf));
        return count > 0 && WriteBackupFile(fname, buf, count);
    }
    if (!DeserializeRegistry(temp_registry, buf, count))
    {
        FreeRegistry(temp_registry);
        return false;
    }

    if (block < temp_registry.block_count)
        temp_registry.RemoveBlock(block);
    if (!CopyBlockInto(temp_registry, block, *registry.GetBlock(block)))
    {
        FreeRegistry(temp_registry);
        return false;
    }

    count = SerializeRegistry(temp_registry, buf, sizeof(buf));
    bool ok = count > 0 && WriteBackupFile(fname, buf, count);
    FreeRegistry(temp_registry);
    return ok;
}

// Restores one block (or the whole registry when `block` is invalid) from the backup file,
// leaving the other live blocks untouched.
template <typename T>
static bool RecallRegistryBlock(BlockRegistry<T> &registry, uint8_t block, const char *fname)
{
    if (block == INVALID_BLOCK)
    {
        uint8_t buf[MEMORY_BACKUP_CAP];
        uint16_t count = ReadBackupFile(fname, buf, sizeof(buf));
        return count > 0 && DeserializeRegistry(registry, buf, count);
    }
    if (block >= registry.block_count)
        return false;

    uint8_t buf[MEMORY_BACKUP_CAP];
    BlockRegistry<T> temp_registry;
    uint16_t count = ReadBackupFile(fname, buf, sizeof(buf));
    if (count > 0 && !DeserializeRegistry(temp_registry, buf, count))
    {
        FreeRegistry(temp_registry);
        return false;
    }
    if (block >= temp_registry.block_count)
    {
        FreeRegistry(temp_registry);
        return false;
    }

    // Insert the stored block at `block` (shifting the current one right), then drop the old
    // copy. The live block is only released after the stored copy is safely in place.
    bool ok = CopyBlockInto(registry, block, *temp_registry.GetBlock(block));
    FreeRegistry(temp_registry);
    if (!ok)
        return false;
    registry.RemoveBlock(block + 1);
    return true;
}

// Builds the backup-read response payload for one block parsed from a backup file.
// Handles block meta+name, field values and (for keyed) dictionary/entry reads.
// Returns the payload length (0 if the requested entry was not found).
static uint16_t BuildBackupPayload(const BlockIndex *idx,
                                   const uint8_t *name, uint8_t name_length,
                                   uint16_t type, const BlockMeta *map, uint16_t map_count,
                                   const uint8_t *data, uint16_t data_length,
                                   bool keyed, uint8_t *out, uint16_t cap)
{
    uint16_t cursor = 0;
    if (idx->Field == INVALID_INDEX)
    {
        if (cursor + sizeof(BlockIndex) + sizeof(BlockMeta) + name_length > cap) return 0;
        BlockIndex out_index = {idx->Block, INVALID_INDEX, INVALID_INDEX};
        memcpy(out + cursor, &out_index, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
        BlockMeta meta; meta.FlagsAndType = type; meta.Key = INVALID_INDEX; meta.Size = (uint8_t)map_count;
        memcpy(out + cursor, &meta, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
        memcpy(out + cursor, name, name_length); cursor += name_length;
        return cursor;
    }
    if (idx->Field >= map_count) return 0;
    const BlockMeta &fd = map[idx->Field];

    if (!keyed)
    {
        uint16_t offset = 0;
        for (uint16_t key = 0; key < idx->Field; key++) offset += AlignTo4(map[key].Size);
        if (offset + fd.Size > data_length) return 0;
        if (cursor + sizeof(BlockIndex) + sizeof(BlockMeta) + fd.Size > cap) return 0;
        memcpy(out + cursor, idx, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
        memcpy(out + cursor, &fd, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
        memcpy(out + cursor, data + offset, fd.Size); cursor += fd.Size;
        return cursor;
    }

    if (idx->Key == INVALID_INDEX) // dictionary: field meta + key list
    {
        // The dict's entries live at the sum of the preceding fields' aligned
        // sizes within the block's data (same offset rule as the dynamic branch).
        uint16_t base = 0;
        for (uint16_t i = 0; i < idx->Field; i++) base += AlignTo4(map[i].Size);
        if (base + fd.Size > data_length) return 0;
        const uint8_t *dict_data = data + base;

        uint16_t key_offset = 0, key_count = 0;
        while (key_offset + sizeof(BlockMeta) <= fd.Size)
        {
            const BlockMeta *meta = (const BlockMeta *)(dict_data + key_offset);
            uint16_t entry_size = AlignTo4(sizeof(BlockMeta) + meta->Size);
            if (key_offset + entry_size > fd.Size) break;
            key_count++;
            key_offset += entry_size;
        }
        if (cursor + sizeof(BlockIndex) + sizeof(BlockMeta) + key_count > cap) return 0;
        BlockIndex out_index = {idx->Block, idx->Field, INVALID_INDEX};
        memcpy(out + cursor, &out_index, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
        BlockMeta dm = fd; dm.Size = (uint8_t)key_count;
        memcpy(out + cursor, &dm, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
        key_offset = 0;
        while (key_offset + sizeof(BlockMeta) <= fd.Size)
        {
            const BlockMeta *meta = (const BlockMeta *)(dict_data + key_offset);
            uint16_t entry_size = AlignTo4(sizeof(BlockMeta) + meta->Size);
            if (key_offset + entry_size > fd.Size) break;
            out[cursor++] = meta->Key;
            key_offset += entry_size;
        }
        return cursor;
    }

    // Keyed entry lookup.
    uint16_t base = 0;
    for (uint16_t i = 0; i < idx->Field; i++) base += AlignTo4(map[i].Size);
    if (base + fd.Size > data_length) return 0;
    const uint8_t *dict_data = data + base;
    uint16_t key_offset = 0;
    while (key_offset + sizeof(BlockMeta) <= fd.Size)
    {
        const BlockMeta *meta = (const BlockMeta *)(dict_data + key_offset);
        uint16_t entry_size = AlignTo4(sizeof(BlockMeta) + meta->Size);
        if (key_offset + entry_size > fd.Size) break;
        if (meta->Key == idx->Key)
        {
            if (cursor + sizeof(BlockIndex) + sizeof(BlockMeta) + meta->Size > cap) return 0;
            memcpy(out + cursor, idx, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
            memcpy(out + cursor, meta, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
            memcpy(out + cursor, dict_data + key_offset + sizeof(BlockMeta), meta->Size); cursor += meta->Size;
            return cursor;
        }
        key_offset += entry_size;
    }
    return 0;
}

// Walks a backup file buffer to answer a backup-read request without any heap allocation.
// Returns the response payload length (0 if not found).
static uint16_t BackupBlockPayload(const BlockIndex *idx, const uint8_t *buf, uint16_t len,
                                   bool keyed, uint8_t *out, uint16_t cap)
{
    if (len < 2) return 0;
    uint16_t cursor = 2;

    if (idx->Block == INVALID_BLOCK) // summary: number of blocks in the file
    {
        uint16_t count = 0;
        while (cursor < len)
        {
            if (cursor + 1 > len) return 0;
            uint8_t name_length = buf[cursor++];
            if (cursor + name_length > len) return 0;
            cursor += name_length;
            if (cursor + 2 > len) return 0;
            cursor += 2;
            if (cursor + 2 > len) return 0;
            uint16_t map_count; memcpy(&map_count, buf + cursor, 2); cursor += 2;
            uint16_t map_bytes = map_count * sizeof(BlockMeta);
            if (cursor + map_bytes > len) return 0;
            cursor += map_bytes;
            if (cursor + 2 > len) return 0;
            uint16_t data_length; memcpy(&data_length, buf + cursor, 2); cursor += 2;
            if (cursor + data_length > len) return 0;
            cursor += data_length;
            count++;
        }
        if (sizeof(BlockIndex) + 1 > cap) return 0;
        BlockIndex out_index = {INVALID_BLOCK, INVALID_INDEX, INVALID_INDEX};
        memcpy(out, &out_index, sizeof(BlockIndex));
        // Saturate rather than wrap: the summary byte cannot represent > 255 blocks.
        out[sizeof(BlockIndex)] = (count > 0xFF) ? 0xFF : (uint8_t)count;
        return sizeof(BlockIndex) + 1;
    }

    uint16_t block_index = 0;
    while (cursor < len)
    {
        if (cursor + 1 > len) return 0;
        uint8_t name_length = buf[cursor++];
        if (cursor + name_length > len) return 0;
        const uint8_t *name = buf + cursor;
        cursor += name_length;
        if (cursor + 2 > len) return 0;
        uint16_t type; memcpy(&type, buf + cursor, 2); cursor += 2;
        if (cursor + 2 > len) return 0;
        uint16_t map_count; memcpy(&map_count, buf + cursor, 2); cursor += 2;
        uint16_t map_bytes = map_count * sizeof(BlockMeta);
        if (cursor + map_bytes > len) return 0;
        const BlockMeta *map = (const BlockMeta *)(buf + cursor);
        cursor += map_bytes;
        if (cursor + 2 > len) return 0;
        uint16_t data_length; memcpy(&data_length, buf + cursor, 2); cursor += 2;
        if (cursor + data_length > len) return 0;
        const uint8_t *data = buf + cursor;
        cursor += data_length;

        if (block_index == idx->Block)
            return BuildBackupPayload(idx, name, name_length, type, map, map_count, data, data_length, keyed, out, cap);
        block_index++;
    }
    return 0;
}

// Dynamic Memory service (runtime blocks, block numbers local to this service):
// Create (0), Delete (1), Read (2), Write (3), Backup (4), Save (5), Recall (6).
void HandleDynamicMemory(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    if (frame.flags & FLAG_TYPE) return; // Ignore responses

    if (PayloadBytes(frame) < sizeof(BlockIndex)) { DeviceLog("DYNMEM", "short payload (%u B)", (unsigned)PayloadBytes(frame)); return; }
    const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);

    // Single scratch buffer shared by every reply-building case (hoisted so the
    // compiler allocates it once - keeps the DAS's 2 KB stack sane).
    uint8_t payload[MAX_PAYLOAD_SIZE];

    switch (cid)
    {
    case 0: // Create block (BlockIndex + BlockMeta + value/name)
    {
        if (PayloadBytes(frame) < sizeof(BlockIndex) + sizeof(BlockMeta)) { RespondStatus(frame, false); break; }
        const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(frame.payload + sizeof(BlockIndex));
        const uint8_t *value = frame.payload + sizeof(BlockIndex) + sizeof(BlockMeta);
        // The real value length is carried by the descriptor's Size field; the wire
        // payload is padded to 4 bytes, so never derive lengths from payload_len.
        uint16_t value_len = desc->Size;
        uint16_t avail = PayloadBytes(frame) - sizeof(BlockIndex) - sizeof(BlockMeta);
        if (value_len > avail) value_len = avail;

        // An explicit index repurposes a None tombstone slot in place, padding
        // any gap with None blocks (indexes stay stable); INVALID_BLOCK appends.
        if (idx->Block != INVALID_BLOCK)
        {
            while (dynamic_block_registry.block_count < idx->Block + 1)
            {
                if (!dynamic_block_registry.AddBlock(BlockType::None))
                { RespondStatus(frame, false); break; }
            }
            DynamicBlockDescriptor *slot = dynamic_block_registry.GetBlock(idx->Block);
            const bool tombstone = slot && (slot->type == BlockType::None ||
                                           slot->type == BlockType::Deleted);
            if (!slot || !tombstone) { RespondStatus(frame, false); break; }
            slot->type = (BlockType)BlockMetaType(desc->FlagsAndType);
            uint16_t n = value_len > BLOCK_NAME_LEN - 1 ? BLOCK_NAME_LEN - 1 : value_len;
            memcpy(slot->Name, value, n); slot->Name[n] = '\0';
            for (uint16_t i = 0; i < slot->map_count; i++)
            {
                auto &m = slot->map[i];
                m.FlagsAndType = (m.FlagsAndType & BLOCK_META_FLAGS_MASK) |
                                 (uint16_t)DataType::None;
            }
            RespondCreate(frame, idx->Block, *desc, value, n);
            break;
        }

        DynamicBlockDescriptor *block = CreateDynamicBlock((BlockType)BlockMetaType(desc->FlagsAndType), value, value_len);
        if (!block) { RespondStatus(frame, false); break; }
        RespondCreate(frame, dynamic_block_registry.block_count - 1, *desc, value, value_len);
        break;
    }
    case 1: // Delete entry when a field index is given, else the whole block
    {
        DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(idx->Block);
        if (!block) { RespondStatus(frame, false); break; }
        if (idx->Field != INVALID_INDEX)
        {
            // Single-entry delete: mark the slot None IN PLACE - remaining
            // indexes never move (Docs/Data Formats.md None semantics).
            bool ok = idx->Field < block->map_count;
            if (ok)
            {
                auto &m = block->map[idx->Field];
                m.FlagsAndType = (m.FlagsAndType & BLOCK_META_FLAGS_MASK) |
                                 (uint16_t)DataType::None;
            }
            RespondStatus(frame, ok);
            break;
        }
        // Docs: deletion marks the type (None); nothing moves, storage is
        // reclaimed on save.
        block->type = BlockType::None;
        RespondStatus(frame, true);
        break;
    }
    case 2: // Read
    {
        if (idx->Block == INVALID_BLOCK) // summary: TOTAL registered blocks
        {
            // Per Docs/Data Formats.md None-as-placeholder semantics: indexes
            // never move - tombstoned (None) slots are reported too and stay
            // addressable until save compacts them away.
            const uint16_t visible = dynamic_block_registry.block_count;
            uint8_t payload[sizeof(BlockIndex) + 1];
            BlockIndex out_index = {INVALID_BLOCK, INVALID_INDEX, INVALID_INDEX};
            memcpy(payload, &out_index, sizeof(BlockIndex));
            payload[sizeof(BlockIndex)] = (uint8_t)visible;
            SendResponse(frame, payload, sizeof(BlockIndex) + 1);
            break;
        }
        DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(idx->Block);
        if (!block) { RespondStatus(frame, false); break; }
        // Tombstoned (None) blocks still answer their META read so block
        // indexes stay aligned with the summary count until save compacts
        // them away; their entries are unreachable.
        const bool deleted = block->type == BlockType::None ||
                             block->type == BlockType::Deleted;
        if (deleted && idx->Field != INVALID_INDEX) { RespondStatus(frame, false); break; }
        if (idx->Field == INVALID_INDEX) // block meta + name
        {
            uint16_t plen = MakeBlockMetaPayload(idx->Block,
                                                 deleted ? (uint16_t)BlockType::None
                                                         : (uint16_t)block->type,
                                                 deleted ? 0 : block->map_count,
                                                 block->Name,
                                                 deleted ? 0 : (uint8_t)strlen(block->Name),
                                                 payload, sizeof(payload));
            if (plen == 0) { RespondStatus(frame, false); break; }
            SendResponse(frame, payload, plen);
            break;
        }
        FieldResult field_result = block->Get(idx->Field);
        if (!field_result.Data) { RespondStatus(frame, false); break; }
        uint16_t cursor = 0;
        memcpy(payload + cursor, idx, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
        memcpy(payload + cursor, &field_result.Descriptor, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
        uint16_t size = field_result.Descriptor.Size;
        if (cursor + size > sizeof(payload)) size = sizeof(payload) - cursor;
        memcpy(payload + cursor, field_result.Data, size); cursor += size;
        SendResponse(frame, payload, cursor);
        break;
    }
    case 3: // Write (BlockIndex + BlockMeta + value); creates if it does not exist
    {
        if (PayloadBytes(frame) < sizeof(BlockIndex) + sizeof(BlockMeta)) { RespondStatus(frame, false); break; }
        const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(frame.payload + sizeof(BlockIndex));
        const uint8_t *value = frame.payload + sizeof(BlockIndex) + sizeof(BlockMeta);
        uint16_t value_len = desc->Size;
        uint16_t avail = PayloadBytes(frame) - sizeof(BlockIndex) - sizeof(BlockMeta);
        if (value_len > avail) value_len = avail;

        if (idx->Block == INVALID_BLOCK) // create new block named by the value
        {
            DynamicBlockDescriptor *block = CreateDynamicBlock((BlockType)BlockMetaType(desc->FlagsAndType), value, value_len);
            if (block) RespondCreate(frame, dynamic_block_registry.block_count - 1, *desc, value, value_len);
            else RespondStatus(frame, false);
            break;
        }
        DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(idx->Block);
        if (!block || block->type == BlockType::Deleted) { RespondStatus(frame, false); break; }

        if (idx->Field == INVALID_INDEX) // set block name and/or type
        {
            // Both are user editable per Docs/Services/Dynamic Memory.md.
            block->type = (BlockType)BlockMetaType(desc->FlagsAndType);
            uint16_t name_len = value_len;
            if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;
            memcpy(block->Name, value, name_len); block->Name[name_len] = '\0';
            RespondEcho(frame, idx, *desc, value, name_len);
            break;
        }

        if (idx->Field < block->map_count)
        {
            if (!block->Set(idx->Field, value, value_len, desc->FlagsAndType))
            {
                RespondStatus(frame, false);
                break;
            }
        }
        else if (idx->Field == block->map_count)
        {
            BlockMeta meta = *desc; meta.Size = (uint8_t)value_len;
            if (!block->InsertField(idx->Field, meta) ||
                !block->Set(idx->Field, value, value_len, desc->FlagsAndType))
            {
                RespondStatus(frame, false);
                break;
            }
        }
        else
        {
            // Gap index: pad the missing slots with None placeholders so the
            // requested index lands (Docs/Data Formats.md None = spacer).
            while (block->map_count < idx->Field)
            {
                BlockMeta pad = {}; pad.FlagsAndType = (uint16_t)DataType::None;
                if (!block->InsertField(block->map_count, pad))
                {
                    RespondStatus(frame, false);
                    break;
                }
            }
            if (block->map_count != idx->Field) { RespondStatus(frame, false); break; }
            BlockMeta meta = *desc; meta.Size = (uint8_t)value_len;
            if (!block->InsertField(idx->Field, meta) ||
                !block->Set(idx->Field, value, value_len, desc->FlagsAndType))
            {
                RespondStatus(frame, false);
                break;
            }
        }
        // Success echo: the request payload already IS BlockIndex + BlockMeta + value.
        SendResponse(frame, frame.payload, PayloadBytes(frame));
        break;
    }
    case 4: // Read backup (direct file parse, no heap)
    {
        uint8_t buf[MEMORY_BACKUP_CAP];
        uint16_t count = ReadBackupFile(DynamicBackupName(), buf, sizeof(buf));
        if (count == 0) { RespondStatus(frame, false); break; }
        uint16_t plen = BackupBlockPayload(idx, buf, count, false, payload, sizeof(payload));
        if (plen == 0) { RespondStatus(frame, false); break; }
        SendResponse(frame, payload, plen);
        break;
    }
    case 5: // Save block (or everything when the block is invalid) to its backup file
    {
        if (SaveRegistryBlock(dynamic_block_registry, idx->Block, DynamicBackupName()))
        {
            PurgeDeleted(dynamic_block_registry);
            RespondStatus(frame, true);
        }
        else
        {
            RespondStatus(frame, false);
        }
        break;
    }
    case 6: // Recall: restore a block (or the whole registry when the block is invalid)
    {
        RespondStatus(frame, RecallRegistryBlock(dynamic_block_registry, idx->Block, DynamicBackupName()));
        break;
    }
    default:
        break;
    }
}

#endif // USE_DYNAMIC_MEMORY

