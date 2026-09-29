#pragma once

// CID 3/4 save+recall and the 0x10-0x15 dynamic management.
//
// Part of Core/Services/Register.h (included from there).

#include "Core/Services/RegisterDefs.h"

// ===== CID 3,4: Save/Recall for Static =====

// Writes (or updates) one field entry in the STATLOG mirror buffer. Entry format:
// BlockIndex[4] + BlockMeta[4] + value[4-aligned]. Returns the new buffer length (0 = no
// room). An existing entry for the same (block, field) is REMOVED first (the tail is shifted
// down) and the new entry appended, so updating an entry in the middle keeps every entry
// that follows it. (Replacing in place returned a length ending at the updated entry, and the
// caller writes back exactly that many bytes - which silently truncated the rest of the log
// and dropped the saved settings of later blocks/instances.)
static uint16_t LogEntryWrite(uint8_t block_idx, uint8_t field, uint8_t *buf, uint16_t cnt,
                              const BlockMeta &m, const uint8_t *val, uint8_t vsz) {
    for (uint16_t c = 0; c + kLogEntryHeaderSize <= cnt; ) {
        uint8_t b = buf[c]; if (b == 0xFF) break;
        uint8_t sz = buf[c + kLogEntryHeaderSize - 1];
        uint16_t el = LogEntrySize(sz);
        if (c + el > cnt) break;
        if (buf[c] == block_idx && buf[c + 1] == field) {
            memmove(buf + c, buf + c + el, cnt - (c + el)); // drop it, close the gap
            cnt -= el;
            break;
        }
        c += el;
    }

    uint16_t need = kLogEntryHeaderSize + ((vsz + 3) & ~3);
    if (cnt + need > MEMORY_BACKUP_CAP) return 0;
    BlockIndex ei = {block_idx, field, 0xFF, 0};
    memcpy(buf + cnt, &ei, sizeof(BlockIndex)); cnt += sizeof(BlockIndex);
    memcpy(buf + cnt, &m, sizeof(BlockMeta));   cnt += sizeof(BlockMeta);
    memcpy(buf + cnt, val, vsz);                cnt += vsz;
    while (cnt % 4) buf[cnt++] = 0;
    return cnt;
}

// Persists one static-block field into the STATLOG buffer.
static uint16_t StaticFieldSave(uint8_t idx, uint8_t field, uint8_t *buf, uint16_t cnt) {
    const StaticBlockDescriptor &blk = static_block_registry[idx];
    FieldResult fr = blk.Get(field);
    if (!fr.Data) return 0;
    return LogEntryWrite(idx, field, buf, cnt, fr.Descriptor, (const uint8_t *)fr.Data, fr.Descriptor.Size);
}

// Recalls one static-block field from the STATLOG buffer into its RAM backing.
static bool StaticFieldRecall(uint8_t idx, uint8_t field, const uint8_t *buf, uint16_t cnt) {
    for (uint16_t c = 0; c + kLogEntryHeaderSize <= cnt; ) {
        uint8_t b = buf[c]; if (b == 0xFF) break;
        uint8_t sz = buf[c + kLogEntryHeaderSize - 1];
        uint16_t el = LogEntrySize(sz);
        if (c + el > cnt) break;
        if (buf[c] == idx && buf[c + 1] == field) {
            FieldResult fr = static_block_registry[idx].Get(field);
            if (fr.Data && sz == fr.Descriptor.Size) {
                memcpy(fr.Data, buf + c + kLogEntryHeaderSize, sz);
                // The RAM value matches the backup again, so it is no longer "not saved".
                StaticDirtySet(idx, field, false);
                return true;
            }
            return false;
        }
        c += el;
    }
    return false;
}

// Persists one System-block persistent field (Name 6 / NetID 7) into the STATLOG buffer.
static uint16_t SystemFieldSave(uint8_t field, uint8_t *buf, uint16_t cnt) {
    uint8_t val[16]; uint8_t vsz = 0; uint16_t type = 0;
    if (field == SYSTEM_FIELD_NAME) {
        vsz = (uint8_t)strlen(DeviceName);
        if (vsz > 16) vsz = 16; // docs: Name is a 16-byte field
        memcpy(val, DeviceName, vsz);
        type = (uint16_t)DataType::String | FieldFlags::Persistent;
#ifdef TYPE_CORE
    } else if (field == SYSTEM_FIELD_NETID) {
        val[0] = DeviceStatus.NetId; vsz = 1;
        type = (uint16_t)DataType::Id | FieldFlags::Persistent;
#endif
    } else {
        return 0;
    }
    BlockMeta m; m.FlagsAndType = type; m.Key = 0xFF; m.Size = vsz;
    return LogEntryWrite(SYSTEM_BLOCK_BACKUP, field, buf, cnt, m, val, vsz);
}

// Recalls a System-block persistent field (Name 6 / NetID 7) from STATLOG into RAM.
static bool SystemFieldRecall(uint8_t field, const uint8_t *buf, uint16_t cnt) {
    for (uint16_t c = 0; c + kLogEntryHeaderSize <= cnt; ) {
        uint8_t b = buf[c]; if (b == 0xFF) break;
        uint8_t sz = buf[c + kLogEntryHeaderSize - 1];
        uint16_t el = LogEntrySize(sz);
        if (c + el > cnt) break;
        if (buf[c] == SYSTEM_BLOCK_BACKUP && buf[c + 1] == field) {
        uint16_t val_c = c + kLogEntryHeaderSize;
        if (field == SYSTEM_FIELD_NAME) {
        uint16_t nl = sz; if (nl > 16) nl = 16;
        memcpy(DeviceNameBuffer, buf + val_c, nl);
        DeviceNameBuffer[nl] = '\0';
#ifdef TYPE_CORE
    } else if (field == SYSTEM_FIELD_NETID) {
        DeviceStatus.NetId = buf[val_c];
#endif
        }
        return true;
        }
        c += el;
    }
    return false;
}

static void HandleStaticSaveRecall(const PacketFrame &frame, uint8_t cid, uint32_t bi_save, uint8_t field) {
    uint16_t type = BlockInfoType(bi_save);
    uint8_t inst = BlockInfoInstance(bi_save);

    uint8_t buf[MEMORY_BACKUP_CAP];
    // The append position is the log's *logical* end, not the file length: the reduced file
    // system pre-allocates the settings file, so a fresh DAS reads back a whole erased region
    // (256 bytes of 0xFF) and every save would otherwise be refused as "buffer full".
    uint16_t cnt = BackupLogUsed(buf, ReadBackupFile(StaticLogName(), buf, sizeof(buf)));

    // System block (type 0, inst 0): its persistent fields (Name 6, NetID 7) live in the
    // SAME static backup file (STATLOG) as the static blocks.
    if (type == 0 && inst == 0) {
        if (cid == 3) { // Save
            uint16_t len = 0;
            bool any = false;
            if (field == SYSTEM_FIELD_NAME || field == 0xFF) {
                uint16_t l = SystemFieldSave(SYSTEM_FIELD_NAME, buf, len ? len : cnt);
                if (l == 0) { RespondStatus(frame,false); return; }
                len = l; any = true;
                StaticDirtySet(SYSTEM_BLOCK_BACKUP, SYSTEM_FIELD_NAME, false); // now in flash
            }
#ifdef TYPE_CORE
            if (field == SYSTEM_FIELD_NETID || field == 0xFF) {
                uint16_t l = SystemFieldSave(SYSTEM_FIELD_NETID, buf, len ? len : cnt);
                if (l == 0) { RespondStatus(frame,false); return; }
                len = l; any = true;
                StaticDirtySet(SYSTEM_BLOCK_BACKUP, SYSTEM_FIELD_NETID, false);
            }
#endif
            if (!any) { RespondStatus(frame,false); return; }
            RespondStatus(frame, WriteBackupFile(StaticLogName(), buf, len));
        } else { // Recall
            bool any = false;
            if (field == SYSTEM_FIELD_NAME || field == 0xFF)
                any |= SystemFieldRecall(SYSTEM_FIELD_NAME, buf, cnt);
#ifdef TYPE_CORE
            if (field == SYSTEM_FIELD_NETID || field == 0xFF)
                any |= SystemFieldRecall(SYSTEM_FIELD_NETID, buf, cnt);
#endif
            RespondStatus(frame, any);
        }
        return;
    }

    int idx = FindStaticBlock(type, inst);
    if (idx < 0) { RespondStatus(frame,false); return; }
    const StaticBlockDescriptor &blk = static_block_registry[idx];

    if (cid == 3) { // Save
        if (field == 0xFF) { // whole block: persist every writable persistent field
            uint16_t len = 0;
            for (uint16_t fi = 0; fi < blk.Schema->MapCount; fi++) {
                if (!(blk.Schema->Map[fi].FlagsAndType & FieldFlags::Persistent) ||
                    (blk.Schema->Map[fi].FlagsAndType & FieldFlags::ReadOnly)) continue;
                uint16_t l = StaticFieldSave((uint8_t)idx, (uint8_t)fi, buf, len ? len : cnt);
                if (l == 0) { RespondStatus(frame,false); return; }
                len = l;
                StaticDirtySet((uint8_t)idx, (uint8_t)fi, false); // now in flash
            }
            if (len == 0) { RespondStatus(frame,true); return; } // nothing writable/persistent
            RespondStatus(frame, WriteBackupFile(StaticLogName(), buf, len));
        } else {
            uint16_t len = StaticFieldSave((uint8_t)idx, field, buf, cnt);
            if (len == 0) { RespondStatus(frame,false); return; }
            StaticDirtySet((uint8_t)idx, field, false); // now in flash
            RespondStatus(frame, WriteBackupFile(StaticLogName(), buf, len));
        }
    } else { // Recall
        bool ok;
        if (field == 0xFF) {
            ok = true;
            for (uint16_t fi = 0; fi < blk.Schema->MapCount; fi++) {
                if (!(blk.Schema->Map[fi].FlagsAndType & FieldFlags::Persistent) ||
                    (blk.Schema->Map[fi].FlagsAndType & FieldFlags::ReadOnly)) continue;
                if (StaticFieldRecall((uint8_t)idx, (uint8_t)fi, buf, cnt)) ok = true;
                else ok = false;
            }
        } else {
            ok = StaticFieldRecall((uint8_t)idx, field, buf, cnt);
        }
        RespondStatus(frame, ok);
    }
}

#ifndef DISABLE_DYNAMIC_MEMORY
static void HandleDynamicSaveRecall(const PacketFrame &frame, uint8_t cid, uint8_t inst_save, uint8_t field) {
    if (cid == 3) { // Save (DT/DV per-block files)
        // Clean tombstoned/orphan files BEFORE writing, so deleted blocks release
        // their storage; positions in the registry are never touched.
        CleanupDynamicFiles();
        if (inst_save == 0x3F) {
            for (uint16_t i = 0; i < dynamic_block_registry.block_count && i < MAX_DYNAMIC_BLOCKS; i++) {
                if (dynamic_block_registry.blocks[i].type == BlockType::None) continue;
                if (!SaveDynamicBlockFiles(dynamic_block_registry.blocks[i], i)) { RespondStatus(frame,false); return; }
            }
        } else {
            if (inst_save >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
            DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(inst_save);
            if (!block || block->type == BlockType::None) { RespondStatus(frame,false); return; }
            if (!SaveDynamicBlockFiles(*block, inst_save)) { RespondStatus(frame,false); return; }
        }
        RespondStatus(frame,true);
        return;
    }

    // Recall (CID 4): rebuild the slot(s) from their DT/DV files. Per the docs each
    // block keeps its own files, so a slot with no DT file stays a tombstone.
    if (inst_save == 0x3F) {
        bool ok = true;
        for (uint16_t i = 0; i < MAX_DYNAMIC_BLOCKS; i++) {
            DynamicBlockDescriptor scratch;
            if (!LoadDynamicBlockFiles(scratch, i))
                continue;
            while (dynamic_block_registry.block_count <= i)
                if (!dynamic_block_registry.AddBlock(BlockType::Undefined)) { ok = false; break; }
            if (!ok) break;
            dynamic_block_registry.TombstoneBlock(i);
            *dynamic_block_registry.GetBlock(i) = scratch;
        }
        RespondStatus(frame, ok);
        return;
    }

    DynamicBlockDescriptor scratch;
    if (!LoadDynamicBlockFiles(scratch, inst_save)) { RespondStatus(frame,false); return; }
    while (dynamic_block_registry.block_count <= inst_save)
        if (!dynamic_block_registry.AddBlock(BlockType::Undefined)) { RespondStatus(frame,false); return; }
    dynamic_block_registry.TombstoneBlock(inst_save);
    *dynamic_block_registry.GetBlock(inst_save) = scratch;
    RespondStatus(frame,true);
}

static void HandleCreateDynamic(const PacketFrame &frame, uint8_t index, uint16_t type) {
    if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
    uint16_t name_len = PayloadBytes(frame) - 4;
    if (name_len > BLOCK_NAME_LEN - 1) name_len = BLOCK_NAME_LEN - 1;
    DynamicBlockDescriptor *block = CreateDynamicBlock((BlockType)type, frame.payload + 4, name_len, index);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + 1];
    BlockIndex out_index = {index, 0xFF, 0xFF};
    memcpy(payload, &out_index, sizeof(BlockIndex));
    payload[sizeof(BlockIndex)] = 1;
    SendResponse(frame, payload, sizeof(payload));
}

static void HandleDeleteDynamic(const PacketFrame &frame, uint16_t block_idx, uint8_t field_idx, uint8_t key) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    if (field_idx == INVALID_INDEX) { // delete the whole block -> tombstone (slot kept)
        dynamic_block_registry.TombstoneBlock(block_idx);
        RespondStatus(frame, true);
        return;
    }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    if (key != INVALID_INDEX) { RespondStatus(frame, block->DeleteEntry(field_idx, key)); return; }
    // Delete every entry at `field` (the record compacts per removal).
    uint8_t keys[256];
    uint16_t n = block->ListKeys(field_idx, keys, 256);
    bool ok = true;
    for (uint16_t i = 0; i < n; i++)
        ok &= block->DeleteEntry(field_idx, keys[i]);
    RespondStatus(frame, ok);
}

static void HandleGetName(const PacketFrame &frame, uint32_t bi, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + BLOCK_NAME_LEN];
    memcpy(payload, &bi, 4);
    uint16_t n = strlen(block->Name); if (n > BLOCK_NAME_LEN - 1) n = BLOCK_NAME_LEN - 1;
    memcpy(payload + 4, block->Name, n);
    SendResponse(frame, payload, 4 + n);
}

static void HandleSetName(const PacketFrame &frame, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    if (PayloadBytes(frame) < 8) { RespondStatus(frame,false); return; }
    uint16_t n = PayloadBytes(frame) - 4; if (n > BLOCK_NAME_LEN - 1) n = BLOCK_NAME_LEN - 1;
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    memcpy(block->Name, frame.payload + 4, n);
    block->Name[n] = '\0';
    RespondStatus(frame, true);
}

static void HandleGetMemUsage(const PacketFrame &frame, uint32_t bi, uint16_t block_idx) {
    if (block_idx >= dynamic_block_registry.block_count) { RespondStatus(frame,false); return; }
    DynamicBlockDescriptor *block = dynamic_block_registry.GetBlock(block_idx);
    if (!block) { RespondStatus(frame,false); return; }
    uint8_t payload[sizeof(BlockIndex) + 24];
    memcpy(payload, &bi, 4);
    uint32_t *u32 = reinterpret_cast<uint32_t *>(payload + 4);
    u32[0] = block->entry_count * sizeof(DynamicEntry);
    u32[1] = block->entry_allocated * sizeof(DynamicEntry);
    u32[2] = block->volatile_len;
    u32[3] = block->volatile_allocated;
    u32[4] = block->persistent_len;
    u32[5] = block->persistent_allocated;
    SendResponse(frame, payload, sizeof(payload));
}

static void HandleReadBackup(const PacketFrame &frame, uint32_t bi, uint16_t block_idx) {
    if (block_idx >= MAX_DYNAMIC_BLOCKS) { RespondStatus(frame,false); return; }
    uint8_t req_field = BlockInfoField(bi);
    uint8_t req_key = BlockInfoKey(bi);

    DynamicBlockDescriptor scratch;
    if (!LoadDynamicBlockFiles(scratch, block_idx)) { RespondStatus(frame,false); return; }

    ReplyDynamicBlockOrField(frame, bi, scratch, req_field, req_key);
    scratch.Release();
}

#endif

