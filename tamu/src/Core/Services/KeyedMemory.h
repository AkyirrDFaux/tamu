#pragma once

#ifdef USE_KEYED_MEMORY

#include "Core/Services/DynamicMemory.h"

// Keyed Memory block registry (runtime allocated).
KeyedRegistry keyed_block_registry;

// Encoded Storage file name holding this service's backup registry.
static const char *KeyedBackupName()
{
    static constexpr char name[8] = {'K', 'E', 'Y', 'M', 'E', 'M', ' ', ' '};
    return name;
}

// Creates a new keyed block of `type` named by `name` (name_len bytes).
static KeyedBlockDescriptor *CreateKeyedBlock(BlockType type, const uint8_t *name, uint16_t name_len)
{
    if (!keyed_block_registry.AddBlock(type))
        return nullptr;
    KeyedBlockDescriptor &block = *keyed_block_registry.GetBlock(keyed_block_registry.block_count - 1);
    uint16_t len = name_len;
    if (len > BLOCK_NAME_LEN - 1) len = BLOCK_NAME_LEN - 1;
    if (len && name) memcpy(block.Name, name, len);
    block.Name[len] = '\0';
    return &block;
}

// Keyed Memory service (runtime blocks + dictionaries, block numbers local to this service):
// Create (0), Delete (1), Read (2), Write (3), Backup (4), Save (5), Recall (6).
void HandleKeyedMemory(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    if (frame.flags & FLAG_TYPE) return; // Ignore responses

    if (PayloadBytes(frame) < sizeof(BlockIndex)) { DeviceLog("KEYMEM", "short payload (%u B)", (unsigned)PayloadBytes(frame)); return; }
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
            while (keyed_block_registry.block_count < idx->Block + 1)
            {
                if (!keyed_block_registry.AddBlock(BlockType::None))
                { RespondStatus(frame, false); break; }
            }
            KeyedBlockDescriptor *slot = keyed_block_registry.GetBlock(idx->Block);
            const bool tombstone = slot && (slot->type == BlockType::None ||
                                           slot->type == BlockType::Deleted);
            if (!slot || !tombstone) { RespondStatus(frame, false); break; }
            slot->type = (BlockType)BlockMetaType(desc->FlagsAndType);
            uint16_t n = value_len > BLOCK_NAME_LEN - 1 ? BLOCK_NAME_LEN - 1 : value_len;
            memcpy(slot->Name, value, n); slot->Name[n] = '\0';
            // Any leftover entries were unreachable under the tombstone; leave
            // them as None placeholders so dict indices stay stable too.
            for (uint16_t i = 0; i < slot->map_count; i++)
            {
                auto &m = slot->map[i];
                m.FlagsAndType = (m.FlagsAndType & BLOCK_META_FLAGS_MASK) |
                                 (uint16_t)DataType::None;
            }
            RespondCreate(frame, idx->Block, *desc, value, n);
            break;
        }

        KeyedBlockDescriptor *block = CreateKeyedBlock((BlockType)BlockMetaType(desc->FlagsAndType), value, value_len);
        if (!block) { RespondStatus(frame, false); break; }
        RespondCreate(frame, keyed_block_registry.block_count - 1, *desc, value, value_len);
        break;
    }
    case 1: // Delete: mark None IN PLACE; reclaimed on save (Docs/Data Formats.md)
    {
        KeyedBlockDescriptor *block = keyed_block_registry.GetBlock(idx->Block);
        if (!block) { RespondStatus(frame, false); break; }
        if (idx->Field == INVALID_INDEX) // whole block
        {
            block->type = BlockType::None;
            RespondStatus(frame, true);
            break;
        }
        if (idx->Field >= block->map_count) { RespondStatus(frame, false); break; }
        if (idx->Key == INVALID_INDEX) // whole dictionary: mark its meta None
        {
            auto &m = block->map[idx->Field];
            m.FlagsAndType = (m.FlagsAndType & BLOCK_META_FLAGS_MASK) |
                             (uint16_t)DataType::None;
            RespondStatus(frame, true);
            break;
        }
        // single keyed entry: mark its meta None in place (no shift)
        RespondStatus(frame, block->MarkKey(idx->Field, idx->Key));
        break;
    }
    case 2: // Read
    {
        if (idx->Block == INVALID_BLOCK) // summary: TOTAL registered blocks
        {
            const uint16_t visible = keyed_block_registry.block_count;
            uint8_t payload[sizeof(BlockIndex) + 1];
            BlockIndex out_index = {INVALID_BLOCK, INVALID_INDEX, INVALID_INDEX};
            memcpy(payload, &out_index, sizeof(BlockIndex));
            payload[sizeof(BlockIndex)] = (uint8_t)visible;
            SendResponse(frame, payload, sizeof(BlockIndex) + 1);
            break;
        }
        KeyedBlockDescriptor *block = keyed_block_registry.GetBlock(idx->Block);
        if (!block) { RespondStatus(frame, false); break; }
        if (idx->Field == INVALID_INDEX) // block meta + name
        {
            uint16_t plen = MakeBlockMetaPayload(idx->Block, (uint16_t)block->type,
                                                 block->map_count, block->Name,
                                                 (uint8_t)strlen(block->Name),
                                                 payload, sizeof(payload));
            if (plen == 0) { RespondStatus(frame, false); break; }
            SendResponse(frame, payload, plen);
            break;
        }
        if (idx->Field >= block->map_count) { RespondStatus(frame, false); break; }
        FieldResult field_result = block->Get(idx->Field);
        // NOTE: an EMPTY dictionary has no backing storage yet (Data == null)
        // but its metadata is still valid, so validity is checked via
        // map_count above - not via field_result.Data.

        // A None-marked dictionary is a deleted placeholder: report it as
        // empty so indexes stay aligned without exposing its old contents.
        const bool dictGone = BlockMetaType(field_result.Descriptor.FlagsAndType) ==
                              (uint16_t)DataType::None;

        if (idx->Key == INVALID_INDEX) // dictionary: BlockMeta + keys array
        {
            // A dictionary holds up to 256 keys (key ids 0..255). The response payload
            // cannot carry all of them at once, so the copy is clamped to what fits.
            uint8_t keys[256];
            uint16_t key_count = dictGone ? 0 : block->ListKeys(idx->Field, keys, sizeof(keys));
            uint16_t cursor = 0;
            BlockIndex out_index = {idx->Block, idx->Field, INVALID_INDEX};
            memcpy(payload + cursor, &out_index, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
            // ListKeys fills at most `sizeof(keys)` entries but counts all of them; the
            // response payload caps what can be sent in one packet (255 - headers).
            uint16_t keys_len = (key_count > sizeof(keys)) ? sizeof(keys) : key_count;
            uint16_t payload_space = sizeof(payload) - cursor - sizeof(BlockMeta);
            if (keys_len > payload_space) keys_len = payload_space;
            BlockMeta desc_meta = field_result.Descriptor; desc_meta.Size = (uint8_t)keys_len;
            memcpy(payload + cursor, &desc_meta, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
            memcpy(payload + cursor, keys, keys_len); cursor += keys_len;
            SendResponse(frame, payload, cursor);
            break;
        }
        KeyResult key_result = block->GetKey(idx->Field, idx->Key); // keyed entry
        if (dictGone || !key_result.data_ptr) { RespondStatus(frame, false); break; }
        uint16_t cursor = 0;
        memcpy(payload + cursor, idx, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
        memcpy(payload + cursor, &key_result.meta, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
        uint16_t size = key_result.data_len;
        if (cursor + size > sizeof(payload)) size = sizeof(payload) - cursor;
        memcpy(payload + cursor, key_result.data_ptr, size); cursor += size;
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
            KeyedBlockDescriptor *block = CreateKeyedBlock((BlockType)BlockMetaType(desc->FlagsAndType), value, value_len);
            if (block) RespondCreate(frame, keyed_block_registry.block_count - 1, *desc, value, value_len);
            else RespondStatus(frame, false);
            break;
        }
        KeyedBlockDescriptor *block = keyed_block_registry.GetBlock(idx->Block);
        const bool blockGone = !block || block->type == BlockType::None ||
                               block->type == BlockType::Deleted;
        if (blockGone) {
            if (block && idx->Field == INVALID_INDEX) {
                // Tombstone meta so indexes stay aligned until save.
                uint16_t plen = MakeBlockMetaPayload(
                    idx->Block, (uint16_t)BlockType::None, 0, block->Name, 0,
                    payload, sizeof(payload));
                if (plen) { SendResponse(frame, payload, plen); break; }
            }
            RespondStatus(frame, false);
            break;
        }

        if (idx->Field == INVALID_INDEX) // set block name and/or type
        {
            // Both are user editable per Docs/Services/Keyed Memory.md.
            block->type = (BlockType)BlockMetaType(desc->FlagsAndType);
            uint16_t name_len = value_len;
            if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;
            memcpy(block->Name, value, name_len); block->Name[name_len] = '\0';
            RespondEcho(frame, idx, *desc, value, name_len);
            break;
        }

        // Ensure the dictionary field exists (padding any gap with None placeholders
        // so an explicit index lands - Docs/Data Formats.md None = spacer).
        if (idx->Field >= block->map_count)
        {
            while (block->map_count < idx->Field)
            {
                BlockMeta pad = {}; pad.FlagsAndType = (uint16_t)DataType::None;
                if (!block->InsertField(block->map_count, pad))
                {
                    RespondStatus(frame, false);
                    break;
                }
            }
            if (block->map_count < idx->Field) { RespondStatus(frame, false); break; }
            if (block->map_count == idx->Field)
            {
                BlockMeta meta = *desc; meta.Size = 0;
                if (!block->InsertField(idx->Field, meta))
                {
                    RespondStatus(frame, false);
                    break;
                }
            }
        }

        if (idx->Key == INVALID_INDEX) // dictionary itself: update its type only
        {
            // Filling a deleted (None) dictionary also clears its stale
            // entries, so the re-added dictionary starts empty.
            if (BlockMetaType(block->map[idx->Field].FlagsAndType) == (uint16_t)DataType::None)
            {
                block->Remove(idx->Field);
                BlockMeta fresh = {}; fresh.FlagsAndType = desc->FlagsAndType;
                if (!block->InsertField(idx->Field, fresh))
                {
                    RespondStatus(frame, false);
                    break;
                }
            }
            else
            {
                block->map[idx->Field].FlagsAndType = desc->FlagsAndType;
            }
            // Request payload already IS the echo (BlockIndex + BlockMeta + value).
            SendResponse(frame, frame.payload, PayloadBytes(frame));
            break;
        }

        if (!block->SetKey(idx->Field, idx->Key, value, (uint8_t)value_len, desc->FlagsAndType))
        {
            RespondStatus(frame, false);
            break;
        }
        // Success echo: the request payload already IS BlockIndex + BlockMeta + value.
        SendResponse(frame, frame.payload, PayloadBytes(frame));
        break;
    }
    case 4: // Read backup (direct file parse, no heap)
    {
        uint8_t buffer[MEMORY_BACKUP_CAP];
        uint16_t count = ReadBackupFile(KeyedBackupName(), buffer, sizeof(buffer));
        if (count == 0) { RespondStatus(frame, false); break; }
        uint16_t plen = BackupBlockPayload(idx, buffer, count, true, payload, sizeof(payload));
        if (plen == 0) { RespondStatus(frame, false); break; }
        SendResponse(frame, payload, plen);
        break;
    }
    case 5: // Save block (or everything when the block is invalid) to its backup file
    {
        if (SaveRegistryBlock(keyed_block_registry, idx->Block, KeyedBackupName()))
        {
            PurgeDeleted(keyed_block_registry);
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
        RespondStatus(frame, RecallRegistryBlock(keyed_block_registry, idx->Block, KeyedBackupName()));
        break;
    }
    case 7: // Read all entries of a dictionary in ONE round trip
    {
        KeyedBlockDescriptor *block = keyed_block_registry.GetBlock(idx->Block);
        if (!block || idx->Field == INVALID_INDEX || idx->Field >= block->map_count)
        { RespondStatus(frame, false); break; }
        FieldResult field_result = block->Get(idx->Field);
        if (BlockMetaType(field_result.Descriptor.FlagsAndType) == (uint16_t)DataType::None)
        { RespondStatus(frame, false); break; }

        uint16_t cursor = 0;
        BlockIndex out_index = {idx->Block, idx->Field, INVALID_INDEX};
        memcpy(payload + cursor, &out_index, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);

        // Count visible entries first (None-marked keys are skipped).
        uint16_t visible = 0;
        if (field_result.Data)
        {
            uint8_t *p = static_cast<uint8_t *>(field_result.Data);
            uint16_t off = 0;
            while (off + sizeof(BlockMeta) <= field_result.Descriptor.Size)
            {
                BlockMeta *m = reinterpret_cast<BlockMeta *>(p + off);
                if (!KeyedEntryFits(m->Size, off, field_result.Descriptor.Size)) break;
                if (((uint16_t)m->FlagsAndType & 0x03FF) != (uint16_t)DataType::None)
                    visible++;
                off += AlignTo4(sizeof(BlockMeta) + m->Size);
            }
        }
        BlockMeta dict_meta = field_result.Descriptor;
        dict_meta.Size = (uint8_t)visible;
        memcpy(payload + cursor, &dict_meta, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);

        // Stream entry metas + values, aligned like the on-disk layout.
        if (field_result.Data)
        {
            uint8_t *p = static_cast<uint8_t *>(field_result.Data);
            uint16_t off = 0;
            while (off + sizeof(BlockMeta) <= field_result.Descriptor.Size)
            {
                BlockMeta *m = reinterpret_cast<BlockMeta *>(p + off);
                if (!KeyedEntryFits(m->Size, off, field_result.Descriptor.Size)) break;
                if (((uint16_t)m->FlagsAndType & 0x03FF) != (uint16_t)DataType::None)
                {
                    uint16_t entry_size = sizeof(BlockMeta) + m->Size;
                    if (cursor + entry_size > sizeof(payload)) break;
                    memcpy(payload + cursor, m, entry_size); cursor += entry_size;
                }
                off += AlignTo4(sizeof(BlockMeta) + m->Size);
            }
        }
        SendResponse(frame, payload, cursor);
        break;
    }
    default:
        break;
    }
}

#endif // USE_KEYED_MEMORY

