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

// Saves the whole STATLOG mirror in one pass: the System block's persistent fields, then
// every registry block's writable persistent fields, written with a single WriteBackupFile.
//
// One pass, not one write per block: WriteBackupFile replaces the file with exactly the bytes
// it is given, and the read buffer is MEMORY_BACKUP_CAP, so a per-block walk that reads a
// truncated file and writes it back would silently drop the entries past the cap. Building the
// whole log in one buffer also makes "does it fit" an explicit failure instead.
static bool StaticSaveAll() {
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t len = SystemFieldSave(SYSTEM_FIELD_NAME, buf, 0);
    if (len == 0) return false;
    uint16_t l = 0;
#ifdef TYPE_CORE
    l = SystemFieldSave(SYSTEM_FIELD_NETID, buf, len);
    if (l == 0) return false;
    len = l;
#endif
    for (size_t i = 0; i < static_block_num; i++) {
        const StaticBlockDescriptor &blk = static_block_registry[i];
        for (uint16_t fi = 0; fi < blk.Schema->MapCount; fi++) {
            if (!(blk.Schema->Map[fi].FlagsAndType & FieldFlags::Persistent) ||
                (blk.Schema->Map[fi].FlagsAndType & FieldFlags::ReadOnly)) continue;
            l = StaticFieldSave((uint8_t)i, (uint8_t)fi, buf, len);
            if (l == 0) return false; // buffer full: the set does not fit MEMORY_BACKUP_CAP
            len = l;
        }
    }
    return WriteBackupFile(StaticLogName(), buf, len);
}

// Recalls the whole STATLOG mirror: one read, then every System and registry persistent field.
// A field with no stored entry (or one whose stored size no longer matches) is left alone -
// that is not a recall failure.
static void StaticRecallAll() {
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t cnt = BackupLogUsed(buf, ReadBackupFile(StaticLogName(), buf, sizeof(buf)));
    SystemFieldRecall(SYSTEM_FIELD_NAME, buf, cnt);
#ifdef TYPE_CORE
    SystemFieldRecall(SYSTEM_FIELD_NETID, buf, cnt);
#endif
    for (size_t i = 0; i < static_block_num; i++) {
        const StaticBlockDescriptor &blk = static_block_registry[i];
        for (uint16_t fi = 0; fi < blk.Schema->MapCount; fi++) {
            if (!(blk.Schema->Map[fi].FlagsAndType & FieldFlags::Persistent) ||
                (blk.Schema->Map[fi].FlagsAndType & FieldFlags::ReadOnly)) continue;
            StaticFieldRecall((uint8_t)i, (uint8_t)fi, buf, cnt);
        }
    }
}

#ifndef DISABLE_DYNAMIC_MEMORY
// Saves every dynamic block to its DT_/DV_ files (the dynamic half of "Save All").
static bool DynamicSaveAll() {
    // Clean tombstoned/orphan files BEFORE writing, so deleted blocks release their storage;
    // positions in the registry are never touched.
    CleanupDynamicFiles();
    for (uint16_t i = 0; i < dynamic_block_registry.block_count && i < MAX_DYNAMIC_BLOCKS; i++) {
        if (dynamic_block_registry.blocks[i].type == BlockType::None) continue;
        if (!SaveDynamicBlockFiles(dynamic_block_registry.blocks[i], i)) return false;
    }
    return true;
}

// Rebuilds every slot from its DT/DV files. Per the docs each block keeps its own files, so
// a slot with no DT file stays a tombstone.
static bool DynamicRecallAll() {
    bool ok = true;
    for (uint16_t i = 0; i < MAX_DYNAMIC_BLOCKS; i++) {
        DynamicBlockDescriptor scratch;
        if (!LoadDynamicBlockFiles(scratch, i)) continue;
        while (dynamic_block_registry.block_count <= i)
            if (!dynamic_block_registry.AddBlock(BlockType::Undefined)) { ok = false; break; }
        if (!ok) break;
        dynamic_block_registry.TombstoneBlock(i);
        *dynamic_block_registry.GetBlock(i) = scratch;
    }
    return ok;
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
    // Six little-endian u32s at payload + 4, 8, ... (a byte array is only 1-aligned).
    StoreUnaligned(payload + 4, (uint32_t)(block->entry_count * sizeof(DynamicEntry)));
    StoreUnaligned(payload + 8, (uint32_t)(block->entry_allocated * sizeof(DynamicEntry)));
    StoreUnaligned(payload + 12, (uint32_t)block->volatile_len);
    StoreUnaligned(payload + 16, (uint32_t)block->volatile_allocated);
    StoreUnaligned(payload + 20, (uint32_t)block->persistent_len);
    StoreUnaligned(payload + 24, (uint32_t)block->persistent_allocated);
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

// CID 3 (Recall All) / CID 4 (Save All): the whole device in one command
// (Docs/Services/Register.md - no BlockInfo). Partial saving/recall is the app's job (direct
// file writes / direct register writes), so nothing is addressed here. Each target keeps its
// own bounded read-modify-write - a DAS's whole persistent set does not fit one
// MEMORY_BACKUP_CAP buffer - and the results are aggregated into a single reply.
static void HandleSaveRecallAll(const PacketFrame &frame, bool save) {
    bool ok = true;
    if (save) {
        ok &= StaticSaveAll();
#ifndef DISABLE_DYNAMIC_MEMORY
        ok &= DynamicSaveAll();
#endif
    } else {
        // A recall has no failure mode of its own: a field with no stored entry is simply left
        // as it is (the doc's reply is Success). Only rebuilding a dynamic slot can fail.
        StaticRecallAll();
#ifndef DISABLE_DYNAMIC_MEMORY
        ok = DynamicRecallAll();
#endif
    }
    RespondStatus(frame, ok);
}
