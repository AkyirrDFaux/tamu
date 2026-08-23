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

    if (frame.payload_len < sizeof(BlockIndex)) return;
    const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);

    switch (cid)
    {
    case 0: // Create block (BlockIndex + BlockMeta + value/name)
    {
        if (frame.payload_len < sizeof(BlockIndex) + sizeof(BlockMeta)) { RespondStatus(frame, false); break; }
        const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(frame.payload + sizeof(BlockIndex));
        const uint8_t *value = frame.payload + sizeof(BlockIndex) + sizeof(BlockMeta);
        uint16_t value_len = frame.payload_len - sizeof(BlockIndex) - sizeof(BlockMeta);

        KeyedBlockDescriptor *block = CreateKeyedBlock((BlockType)BlockMetaType(desc->FlagsAndType), value, value_len);
        if (!block) { RespondStatus(frame, false); break; }
        RespondCreate(frame, keyed_block_registry.block_count - 1, *desc, value, value_len);
        break;
    }
    case 1: // Delete block (marked Deleted, deallocated on next Save)
    {
        KeyedBlockDescriptor *block = keyed_block_registry.GetBlock(idx->Block);
        if (block) block->type = BlockType::Deleted;
        RespondStatus(frame, block != nullptr);
        break;
    }
    case 2: // Read
    {
        if (idx->Block == INVALID_BLOCK) // summary: count of visible (non-deleted) blocks
        {
            uint16_t visible = 0;
            for (uint16_t i = 0; i < keyed_block_registry.block_count; i++)
                if (keyed_block_registry.GetBlock(i)->type != BlockType::Deleted)
                    visible++;
            uint8_t payload[sizeof(BlockIndex) + 1];
            BlockIndex out_index = {INVALID_BLOCK, INVALID_INDEX, INVALID_INDEX};
            memcpy(payload, &out_index, sizeof(BlockIndex));
            payload[sizeof(BlockIndex)] = (uint8_t)visible;
            SendResponse(frame, payload, sizeof(BlockIndex) + 1);
            break;
        }
        KeyedBlockDescriptor *block = keyed_block_registry.GetBlock(idx->Block);
        if (!block || block->type == BlockType::Deleted) { RespondStatus(frame, false); break; }
        if (idx->Field == INVALID_INDEX) // block meta + name
        {
            uint8_t payload[MAX_PAYLOAD_SIZE];
            uint16_t plen = MakeBlockMetaPayload(idx->Block, (uint16_t)block->type,
                                                 block->map_count, block->Name,
                                                 (uint8_t)strlen(block->Name),
                                                 payload, sizeof(payload));
            if (plen == 0) { RespondStatus(frame, false); break; }
            SendResponse(frame, payload, (uint8_t)plen);
            break;
        }
        FieldResult field_result = block->Get(idx->Field);
        if (!field_result.Data) { RespondStatus(frame, false); break; }

        if (idx->Key == INVALID_INDEX) // dictionary: BlockMeta + keys array
        {
            // A dictionary holds up to 256 keys (key ids 0..255). The response payload
            // cannot carry all of them at once, so the copy is clamped to what fits.
            uint8_t keys[256];
            uint16_t key_count = block->ListKeys(idx->Field, keys, sizeof(keys));
            uint8_t payload[MAX_PAYLOAD_SIZE];
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
            SendResponse(frame, payload, (uint8_t)cursor);
            break;
        }
        KeyResult key_result = block->GetKey(idx->Field, idx->Key); // keyed entry
        if (!key_result.data_ptr) { RespondStatus(frame, false); break; }
        uint8_t payload[MAX_PAYLOAD_SIZE];
        uint16_t cursor = 0;
        memcpy(payload + cursor, idx, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
        memcpy(payload + cursor, &key_result.meta, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
        uint16_t size = key_result.data_len;
        if (cursor + size > sizeof(payload)) size = sizeof(payload) - cursor;
        memcpy(payload + cursor, key_result.data_ptr, size); cursor += size;
        SendResponse(frame, payload, (uint8_t)cursor);
        break;
    }
    case 3: // Write (BlockIndex + BlockMeta + value); creates if it does not exist
    {
        if (frame.payload_len < sizeof(BlockIndex) + sizeof(BlockMeta)) { RespondStatus(frame, false); break; }
        const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(frame.payload + sizeof(BlockIndex));
        const uint8_t *value = frame.payload + sizeof(BlockIndex) + sizeof(BlockMeta);
        uint16_t value_len = frame.payload_len - sizeof(BlockIndex) - sizeof(BlockMeta);

        if (idx->Block == INVALID_BLOCK) // create new block named by the value
        {
            KeyedBlockDescriptor *block = CreateKeyedBlock((BlockType)BlockMetaType(desc->FlagsAndType), value, value_len);
            if (block) RespondCreate(frame, keyed_block_registry.block_count - 1, *desc, value, value_len);
            else RespondStatus(frame, false);
            break;
        }
        KeyedBlockDescriptor *block = keyed_block_registry.GetBlock(idx->Block);
        if (!block || block->type == BlockType::Deleted) { RespondStatus(frame, false); break; }

        if (idx->Field == INVALID_INDEX) // set block name
        {
            uint16_t name_len = value_len;
            if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;
            memcpy(block->Name, value, name_len); block->Name[name_len] = '\0';
            RespondEcho(frame, idx, *desc, value, name_len);
            break;
        }

        // Ensure the dictionary field exists.
        if (idx->Field == block->map_count)
        {
            BlockMeta meta = *desc; meta.Size = 0;
            if (!block->InsertField(idx->Field, meta))
            {
                RespondStatus(frame, false);
                break;
            }
        }
        if (idx->Field >= block->map_count) { RespondStatus(frame, false); break; }

        if (idx->Key == INVALID_INDEX) // dictionary itself: update its type only
        {
            block->map[idx->Field].FlagsAndType = desc->FlagsAndType;
            // Request payload already IS the echo (BlockIndex + BlockMeta + value).
            SendResponse(frame, frame.payload, frame.payload_len);
            break;
        }

        if (BlockMetaType(desc->FlagsAndType) == (uint16_t)DataType::Deleted)
        {
            bool removed = block->RemoveKey(idx->Field, idx->Key);
            RespondStatus(frame, removed);
            break;
        }

        if (!block->SetKey(idx->Field, idx->Key, value, (uint8_t)value_len, desc->FlagsAndType))
        {
            RespondStatus(frame, false);
            break;
        }
        // Success echo: the request payload already IS BlockIndex + BlockMeta + value.
        SendResponse(frame, frame.payload, frame.payload_len);
        break;
    }
    case 4: // Read backup (direct file parse, no heap)
    {
        uint8_t buffer[MEMORY_BACKUP_CAP];
        uint16_t count = ReadBackupFile(KeyedBackupName(), buffer, sizeof(buffer));
        if (count == 0) { RespondStatus(frame, false); break; }
        uint8_t payload[MAX_PAYLOAD_SIZE];
        uint16_t plen = BackupBlockPayload(idx, buffer, count, true, payload, sizeof(payload));
        if (plen == 0) { RespondStatus(frame, false); break; }
        SendResponse(frame, payload, (uint8_t)plen);
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
    default:
        break;
    }
}

#endif // USE_KEYED_MEMORY

