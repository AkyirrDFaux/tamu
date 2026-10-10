#pragma once

// The full multi-file file system (Tamu).
//
// Part of Core/Functions/Storage.h (included from there).

#include "Core/Functions/StorageDefs.h"

class StorageSystem
{
public:
    uint32_t file_table_offset;  // Offset of the current file table (0 if none)
    uint32_t file_table_size;    // Current file table size in bytes (multiple of PAGE_SIZE)

    // Initializes flash access, recovers the file table and formats if none is valid.
    void Init()
    {
        if (!Storage_FlashInit()) {
            DeviceLog("STORAGE", "Storage flash not available!");
            file_table_offset = 0;
            file_table_size = 0;
            return;
        }

        // An unreadable pointer (FindFiletable returns false) is treated like "no table":
        // the location is unknown, so (re)format rather than trust a partial result.
        if (!FindFiletable(&file_table_offset))
            file_table_offset = 0;
        if (file_table_offset == 0 || !ValidateTable()) {
            Format();
        }
    }

    // Reads the file-table pointer from the first page. Returns false when a flash read
    // fails (the table location is unknown; the caller must not trust a partial result).
    // On success `*offset` is the LAST valid slot (0 when no valid slot exists). Slots
    // are scanned one at a time so a large pointer page (4096 B on the Tamu) never needs
    // a matching stack buffer. The LAST valid slot wins, not the first: WriteTablePointer
    // only ever appends (older slots are never invalidated), so after an interrupted update
    // both can be valid and the newest (highest slot) must win - otherwise a crash would
    // silently boot the stale table while FindSpace considers the new table's pages free.
    bool FindFiletable(uint32_t *offset)
    {
        uint32_t slot = 0;
        uint32_t newest = 0;
        for (uint32_t i = 0; i < PTR_SLOTS; i++) {
            if (Storage_FlashRead(i * 4, &slot, sizeof(slot)) != sizeof(slot))
                return false;
            // Track each valid slot (neither 0x00000000 nor 0xFFFFFFFF); later slots are newer.
            if (slot != 0x00000000 && slot != 0xFFFFFFFF)
                newest = slot;
        }
        *offset = newest;
        return true;
    }

    // Appends a new table pointer to the first page (erasing it only when the last slot is
    // used; low-wear first page). Older slots are left untouched - the newest valid slot wins
    // on read (B13), so invalidating them is only unnecessary flash writes.
    bool WriteTablePointer(uint32_t new_offset)
    {
        uint32_t slot = 0;
        uint32_t write_slot = PTR_SLOTS;
        for (uint32_t i = 0; i < PTR_SLOTS; i++) {
            if (Storage_FlashRead(i * 4, &slot, sizeof(slot)) != sizeof(slot))
                return false;
            if (slot == 0xFFFFFFFF) { // first unused slot
                write_slot = i;
                break;
            }
        }

        if (write_slot == PTR_SLOTS) {
            // Block is full, erase it and start fresh (erased flash is 0xFFFFFFFF = "unused").
            if (!Storage_FlashErase(0, PAGE_SIZE))
                return false;
            return Storage_FlashWrite(0, &new_offset, sizeof(uint32_t));
        }

        // Write new offset to the found slot; older slots are left untouched (newest wins).
        return Storage_FlashWrite(write_slot * 4, &new_offset, sizeof(uint32_t));
    }

    // Finds the NEWEST file record matching `name`. Returns false when a flash read fails
    // (the table is partially unreadable, so no result is trustworthy); on success `*index`
    // is the record index, or 0xFFFFFFFF when none. The newest (highest slot) wins so a
    // duplicate left by an older build cannot shadow the current file (RenameFile appends
    // the replacement).
    bool FindInFiletable(const char name[8], uint32_t *index)
    {
        if (file_table_offset == 0) { *index = 0xFFFFFFFF; return true; }
        uint32_t capacity = TableCapacity();
        uint32_t found = 0xFFFFFFFF;
        for (uint32_t i = 0; i < capacity; i++) {
            FileRecord entry;
            if (!ReadTableEntry(i, &entry))
                return false;
            if (FileRecordIsValid(entry.offset) && NameMatch(entry.name, name))
                found = i;
        }
        *index = found;
        return true;
    }

    // Finds the first non-written file record index (first slot with offset 0xFFFFFFFF).
    // Returns false when a flash read fails; on success `*index` is that slot, or the
    // capacity when the table is full. Invalidated (0x00) holes are never reused, so this
    // is the append position.
    bool GetEndOfFiletable(uint32_t *index)
    {
        if (file_table_offset == 0) { *index = 0; return true; }
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 0; i < capacity; i++) {
            FileRecord entry;
            if (!ReadTableEntry(i, &entry))
                return false;
            if (entry.offset == 0xFFFFFFFF) { *index = i; return true; }
        }
        *index = capacity;
        return true;
    }

    // Writes a new file record at the end of the table. If no space is available, the table
    // is filtered and moved (MoveFiletable), then the write is retried.
    bool WriteFileRecord(const FileRecord &new_record)
    {
        uint32_t slot;
        if (!GetEndOfFiletable(&slot))
            return false;
        if (slot >= TableCapacity()) {
            if (!MoveFiletable())
                return false;
            if (!GetEndOfFiletable(&slot))
                return false;
            if (slot >= TableCapacity())
                return false;
        }
        return Storage_FlashWrite(file_table_offset + slot * TABLE_ENTRY_SIZE,
                                  &new_record, TABLE_ENTRY_SIZE);
    }

    // Invalidates EVERY file record matching `name` in place by zeroing the offset word
    // only (the same 4-byte policy as every other invalidation path and as the app's
    // readFileTable, which skips any record whose offset is 0 on the full file system;
    // the size is left in place). All matches are removed so pre-existing duplicates are
    // fully deleted, not just the newest one.
    template <typename Match>
    bool DeleteMatching(Match matches)
    {
        if (file_table_offset == 0) return true;
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 1; i < capacity; i++) { // entry 0 (the table) is never deleted
            FileRecord entry;
            if (!ReadTableEntry(i, &entry))
                return false;
            if (FileSlotIsFree(entry.offset)) continue;
            if (!matches(entry.name)) continue;
            uint32_t zero = 0x00000000;
            if (!Storage_FlashWrite(file_table_offset + i * TABLE_ENTRY_SIZE, &zero, sizeof(zero)))
                return false;
        }
        return true; // already gone is fine
    }

    bool DeleteFileRecord(const char name[8])
    {
        return DeleteMatching([&](const char *n) { return NameMatch(n, name); });
    }

    // Invalidates every live record whose name satisfies `matches`, except the freshly appended
    // generation at `keep_index`. Zeroes the offset word only (as every invalidation path does),
    // so a replaced generation is never mistaken for the current file.
    template <typename Match>
    bool InvalidateMatchingExcept(Match matches, uint32_t keep_index)
    {
        if (file_table_offset == 0) return true;
        uint32_t capacity = TableCapacity();
        uint32_t zero = 0x00000000;
        for (uint32_t i = 0; i < capacity; i++) {
            if (i == keep_index) continue;
            FileRecord e;
            if (!ReadTableEntry(i, &e))
                return false;
            if (FileSlotIsFree(e.offset)) continue;
            if (!matches(e.name)) continue;
            if (!Storage_FlashWrite(file_table_offset + i * TABLE_ENTRY_SIZE,
                                    &zero, sizeof(zero)))
                return false;
        }
        return true;
    }

    // Calls `fn(name)` once for every live file record. A superseded record for the same
    // name can linger until the next save, so a name may repeat; callers that care dedupe.
    template <typename F>
    void ForEachFile(F fn)
    {
        if (file_table_offset == 0) return;
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 1; i < capacity; i++) { // entry 0 (the table) is not a file
            FileRecord entry;
            if (!ReadTableEntry(i, &entry)) return;
            if (FileSlotIsFree(entry.offset)) continue;
            fn(entry.name);
        }
    }

    // Renames `old_name` to `new_name`: appends a record with the new name for the same
    // data area, then invalidates the superseded records (Docs/Services/Storage.md).
    //
    // Crash-safe by construction - each step is an append-only NOR write or a 1->0
    // record invalidation, so a power cut leaves readers resolving either the old or the
    // new name to a complete file. An existing `new_name` is replaced (its previous data
    // area becomes unreferenced and therefore free for future allocation).
    bool RenameFile(const char old_name[8], const char new_name[8])
    {
        uint32_t src;
        if (!FindInFiletable(old_name, &src) || src == 0xFFFFFFFF || src == 0)
            return false; // read failure / missing / the table itself

        FileRecord entry;
        if (!ReadTableEntry(src, &entry)) return false;

        FileRecord rec = entry;
        // Space-pad the new name: the caller passes a plain C string and a raw
        // memcpy would copy its NUL terminator into the record (breaking later
        // 8-byte memcmp lookups and the app's file-type detection).
        PackName(new_name, rec.name);
        if (!WriteFileRecord(rec))
            return false;

        // The appended record sits at the current end; invalidate every OTHER valid
        // record still carrying either name (the previous generation under each). If the
        // end cannot be re-read the new record stays live and the old one is untouched
        // (no data loss); report the failure.
        uint32_t appended;
        if (!GetEndOfFiletable(&appended) || appended == 0)
            return false;
        return InvalidateMatchingExcept(
            [&](const char *n) { return NameMatch(n, old_name) || NameMatch(n, new_name); },
            appended - 1);
    }

    // Moves the full table into a fresh area of the same, fixed size (B15). Finds a new
    // space, erases it, writes a self-describing entry 0 and copies the valid entries
    // (compacting out the holes), then appends the new first-page pointer.
    bool MoveFiletable()
    {
        uint32_t capacity = TableCapacity();
        if (capacity == 0) return false;

        uint32_t new_size = file_table_size; // fixed per device (STORAGE_TABLE_SIZE)

        uint32_t new_offset = FindSpace(new_size);
        if (new_offset == 0) return false; // Out of space

        if (!Storage_FlashErase(new_offset, new_size))
            return false;

        // Entry 0: self-describing table (points to its own location and length)
        FileRecord entry0;
        memset(&entry0, 0xFF, sizeof(entry0));
        entry0.offset = new_offset;
        entry0.size = new_size;
        memcpy(entry0.name, ".TABLE  ", 8);
        if (!Storage_FlashWrite(new_offset, &entry0, TABLE_ENTRY_SIZE))
            return false;

        // Copy valid entries (dense, skipping holes); entry 0 is regenerated above.
        uint32_t dest = 1;
        for (uint32_t i = 0; i < capacity; i++) {
            if (i == 0) continue;
            FileRecord entry;
            if (!ReadTableEntry(i, &entry))
                return false;
            if (!FileRecordIsValid(entry.offset)) continue;
            if (dest * TABLE_ENTRY_SIZE >= new_size)
                return false;
            if (!Storage_FlashWrite(new_offset + dest * TABLE_ENTRY_SIZE, &entry, TABLE_ENTRY_SIZE))
                return false;
            dest++;
        }

        // Point at the new table (the old pointer slot is left in place; newest wins).
        if (!WriteTablePointer(new_offset))
            return false;

        file_table_offset = new_offset;
        file_table_size = new_size;
        DeviceLog("STORAGE", "Table moved to 0x%x (%d B)", (unsigned)new_offset,
                  (int)new_size);
        return true;
    }

    // Finds contiguous free space of `size` bytes, excluding existing files and the pointer
    // page. Both shapes start at the rotating wear cursor so allocation spreads wear.
    //
    // OPTIMIZE_SPEED builds (the core) build a usage bitmap and pick the best-fitting run:
    // fewest table re-reads and less fragmentation, which is worth the flash where there is
    // room. Size builds (the DAS and any space-constrained target) do a linear first-fit,
    // re-reading the table once per candidate page through BlockUsed; the extra flash reads
    // are negligible on a small device and the code is a fraction of the bitmap shape.
    uint32_t FindSpace(uint32_t size_bytes)
    {
        uint32_t blocks = BlocksForSize(size_bytes);

        uint32_t data_start = DataStart();
        uint32_t data_end = DataEnd();
        if (data_end <= data_start)
            return 0;
        uint32_t num_blocks = (data_end - data_start) / PAGE_SIZE;
        if (blocks > num_blocks)
            return 0;

#ifdef OPTIMIZE_SPEED
        // Build the usage bitmap directly from the file table (~entries, not
        // num_blocks x entries): per-candidate BlockUsed() rescans the whole table each time.
        uint8_t used_bitmap[(STORAGE_MAX_BLOCKS + 7) / 8] = {0};
        auto markUsed = [&](uint32_t start, uint32_t nblocks)
        {
            if (start < data_start || start >= data_end) return; // outside the data area
            uint32_t first = (start - data_start) / PAGE_SIZE;
            for (uint32_t k = 0; k < nblocks && first + k < num_blocks; k++)
                used_bitmap[(first + k) >> 3] |= (uint8_t)(1u << ((first + k) & 7));
        };
        for (uint32_t i = 0; i < TableCapacity(); i++)
        {
            FileRecord entry;
            if (!ReadTableEntry(i, &entry)) return 0; // unreadable table: no space is safe
            if (FileSlotIsFree(entry.offset)) continue;
            uint32_t blocks = BlocksForSize(entry.size);
            if (blocks == 0) blocks = 1;
            markUsed(entry.offset, blocks);
        }
        if (pending_blocks)
            markUsed(pending_offset, pending_blocks);

        uint32_t best_offset = 0;
        uint32_t best_run = 0;

        // Scan free runs starting at the wear cursor (wrapping) so allocation placement
        // rotates across the storage over time, then pick the best-fit run.
        for (uint32_t scanned = 0; scanned < num_blocks; ) {
            uint32_t idx = (wear_cursor + scanned) % num_blocks;
            uint32_t block_offset = data_start + idx * PAGE_SIZE;

            if (used_bitmap[idx >> 3] & (1u << (idx & 7))) {
                scanned++;
                continue;
            }

            // Measure the contiguous free run starting here. The address space is
            // LINEAR (files are a contiguous [offset, offset+blocks*PAGE) range), so a
            // run must NOT wrap past the end of the data area: a wrapped run would
            // allocate pages that physically live at the start of storage, outside the
            // file's linear range - invisible to BlockUsed (double-allocation) and
            // unreadable/unwritable through the file API.
            uint32_t run = 0;
            while (run < num_blocks)
            {
                uint32_t probe = idx + run;
                if (probe >= num_blocks) break;
                if (used_bitmap[probe >> 3] & (1u << (probe & 7)))
                    break;
                run++;
            }

            if (run >= blocks && (best_offset == 0 || run < best_run)) {
                best_offset = block_offset;
                best_run = run;
            }
            scanned += run; // skip the measured run
        }

        if (best_offset == 0)
            return 0;

        // Advance the wear cursor past the allocated run so the next allocation starts later.
        uint32_t idx = (best_offset - data_start) / PAGE_SIZE;
        wear_cursor = (idx + blocks) % num_blocks;
        return best_offset;
#else
        // Size variant: first-fit linear scan from the wear cursor. A run must not wrap past
        // the data end (files are a linear [offset, offset+blocks*PAGE) range).
        for (uint32_t scanned = 0; scanned < num_blocks; scanned++)
        {
            uint32_t idx = (wear_cursor + scanned) % num_blocks;
            if (idx + blocks > num_blocks)
                continue; // would wrap the linear data area
            uint32_t run = 0;
            while (run < blocks && !BlockUsed(data_start + (idx + run) * PAGE_SIZE))
                run++;
            if (run == blocks)
            {
                wear_cursor = (idx + blocks) % num_blocks;
                return data_start + idx * PAGE_SIZE;
            }
        }
        return 0;
#endif
    }

    // Creates a file of `size` bytes; returns true on success.
    bool CreateFile(const char name[8], uint32_t size)
    {
        // A zero-size file cannot exist (Docs/Services/Storage.md: Create File - "Size (>0)").
        if (size == 0) return false;

        // Refuse a duplicate (and any unreadable table: a read failure must not create a
        // second record for a name that might already exist).
        uint32_t existing;
        if (!FindInFiletable(name, &existing) || existing != 0xFFFFFFFF)
            return false;

        uint32_t data_offset = FindSpace(size);
        if (data_offset == 0) return false; // Out of space

        uint32_t data_blocks = BlocksForSize(size);
        if (!Storage_FlashErase(data_offset, data_blocks * PAGE_SIZE))
            return false;

        FileRecord new_record;
        new_record.offset = data_offset;
        new_record.size = size;
        PackName(name, new_record.name);

        // The committing record is not in the table yet. Reserve the area so a table
        // move triggered by WriteFileRecord can never relocate the file table onto
        // this freshly erased (and invisible) space.
        pending_offset = data_offset;
        pending_blocks = data_blocks;
        bool ok = WriteFileRecord(new_record);
        pending_offset = 0;
        pending_blocks = 0;
        return ok;
    }

    // Invalidates the file table entry for `name`; true if it existed (or was already gone).
    bool DeleteFile(const char name[8])
    {
        return DeleteFileRecord(name);
    }

    // Grows or shrinks a file to `new_size`. Shrinking always succeeds in place; growing
    // only succeeds when the run can be extended in place (and stays inside the data area).
    bool ResizeFile(const char name[8], uint32_t new_size)
    {
        // A zero-size file cannot exist (Docs/Services/Storage.md: Resize File - "New Size (>0)").
        if (new_size == 0) return false;

        uint32_t idx;
        if (!FindInFiletable(name, &idx) || idx == 0xFFFFFFFF || idx == 0)
            return false; // read failure / missing / entry 0 (the table itself)

        FileRecord entry;
        if (!ReadTableEntry(idx, &entry))
            return false;

        uint32_t current_blocks = BlocksForSize(entry.size);
        uint32_t new_blocks = BlocksForSize(new_size);

        if (new_blocks <= current_blocks) {
            // Shrink or same size: append the new record, then invalidate the previous
            // generation (and any older duplicate) by index - never DeleteFileRecord, which
            // would also remove the record just appended and orphan the data.
            FileRecord new_record = entry;
            new_record.size = new_size;
            if (!WriteFileRecord(new_record))
                return false;
            uint32_t appended;
            if (!GetEndOfFiletable(&appended) || appended == 0)
                return false;
            return InvalidateMatchingExcept([&](const char *n) { return NameMatch(n, name); },
                                            appended - 1);
        }

        // Enlargement: reject before scanning if the file's linear range would leave the data
        // area. This also bounds the scan below (new_blocks can otherwise be huge).
        if (entry.offset >= DataEnd() || new_blocks > (DataEnd() - entry.offset) / PAGE_SIZE)
            return false;
        bool can_extend = true;
        for (uint32_t b = current_blocks; b < new_blocks; b++) {
            if (BlockUsed(entry.offset + b * PAGE_SIZE)) {
                can_extend = false;
                break;
            }
        }
        if (can_extend) {
            // Erase the newly-added tail pages (they are free, so erasing is safe).
            uint32_t tail_offset = entry.offset + current_blocks * PAGE_SIZE;
            uint32_t tail_blocks = new_blocks - current_blocks;
            if (!Storage_FlashErase(tail_offset, tail_blocks * PAGE_SIZE))
                return false;
            FileRecord new_record = entry;
            new_record.size = new_size;
            // Reserve the erased tail so a table move triggered by WriteFileRecord
            // cannot relocate the table onto it (same rule as CreateFile).
            pending_offset = tail_offset;
            pending_blocks = tail_blocks;
            bool ok = WriteFileRecord(new_record);
            pending_offset = 0;
            pending_blocks = 0;
            if (!ok) return false;
            uint32_t appended;
            if (!GetEndOfFiletable(&appended) || appended == 0)
                return false;
            return InvalidateMatchingExcept([&](const char *n) { return NameMatch(n, name); },
                                            appended - 1);
        }

        // Extending in place is not possible.
        return false;
    }

    // Utility wrapper for reading from a file; offset is from file start. Returns bytes read.
    uint32_t ReadFromFile(const char name[8], uint32_t offset, uint32_t length, char *buffer)
    {
        uint32_t file_offset, file_size;
        if (!GetFileInfo(name, &file_offset, &file_size))
            return 0;
        if (offset >= file_size)
            return 0;
        length = ClampFileLength(offset, length, file_size);
        return Storage_FlashRead(file_offset + offset, buffer, length);
    }

    // Safer wrapper for writing to a file; offset is from file start. Writes are clamped to
    // the file size. Returns true if the write succeeded.
    bool WriteToFile(const char name[8], uint32_t offset, uint32_t length, const char *buffer)
    {
        uint32_t file_offset, file_size;
        if (!GetFileInfo(name, &file_offset, &file_size))
            return false;
        if (offset >= file_size)
            return false;
        length = ClampFileLength(offset, length, file_size);
        return Storage_FlashWrite(file_offset + offset, buffer, length);
    }

    // Returns the file size if it exists, otherwise 0xFFFFFFFF. A read failure is also
    // reported as 0xFFFFFFFF (a caller cannot act on a size it could not read).
    uint32_t FileExists(const char name[8])
    {
        uint32_t idx;
        if (!FindInFiletable(name, &idx) || idx == 0xFFFFFFFF)
            return 0xFFFFFFFF;
        FileRecord entry;
        if (!ReadTableEntry(idx, &entry))
            return 0xFFFFFFFF;
        return entry.size;
    }

    // Erases the pointer page and storage, then initializes an empty self-describing table
    // of the fixed per-device size (STORAGE_TABLE_SIZE) at the first data page (block 1).
    bool Format()
    {
        if (!Storage_FlashFormat()) {
            DeviceLog("STORAGE", "Format failed!");
            return false;
        }

        FileRecord entry0;
        memset(&entry0, 0xFF, sizeof(entry0));
        entry0.offset = PAGE_SIZE;              // Table starts at block 1
        entry0.size = STORAGE_TABLE_SIZE;       // Fixed per-device table capacity
        memcpy(entry0.name, ".TABLE  ", 8);

        if (Storage_FlashWrite(PAGE_SIZE, &entry0, TABLE_ENTRY_SIZE) != true)
            return false;

        if (!WriteTablePointer(PAGE_SIZE))
            return false;

        file_table_offset = PAGE_SIZE;
        file_table_size = STORAGE_TABLE_SIZE;
        DeviceLog("STORAGE", "Formatted, storage ready (%d bytes)", (int)DataEnd());
        return true;
    }

    // Returns the total number of bytes used by files in flash (excluding file table itself).
    uint32_t UsedFlashBytes()
    {
        if (file_table_offset == 0) return 0;
        uint32_t used = 0;
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 1; i < capacity; i++) { // entry 0 is the table itself
            FileRecord entry;
            if (!ReadTableEntry(i, &entry)) continue;
            if (FileSlotIsFree(entry.offset)) continue;
            uint32_t blocks = BlocksForSize(entry.size);
            if (blocks == 0) blocks = 1; // zero-size file still occupies one block
            used += blocks * PAGE_SIZE;
        }
        return used;
    }

    // Looks up a file by its name; returns offset/size via the out params.
    bool GetFileInfo(const char name[8], uint32_t *offset, uint32_t *size)
    {
        uint32_t idx;
        if (!FindInFiletable(name, &idx) || idx == 0xFFFFFFFF) return false;
        FileRecord entry;
        if (!ReadTableEntry(idx, &entry))
            return false;
        *offset = entry.offset;
        *size = entry.size;
        return true;
    }

    // Returns the number of records the current table can hold.
    uint32_t TableCapacity()
    {
        if (file_table_size == 0) return 0;
        return file_table_size / TABLE_ENTRY_SIZE;
    }

private:
    // Pending (reserved) allocation: a data area that has been erased for a new/moved
    // file but whose committing table record is not written yet. Treated as used so a
    // concurrent table move can never be placed over it.
    uint32_t pending_offset = 0;
    uint32_t pending_blocks = 0;

    // True when the page at `offset` overlaps the pending reservation.
    bool RangeIsPending(uint32_t offset) const
    {
        if (pending_blocks == 0)
            return false;
        uint32_t start = pending_offset;
        uint32_t end = pending_offset + pending_blocks * PAGE_SIZE;
        return offset >= start && offset < end;
    }

    // Returns the first flash offset of the file data area (after the pointer page).
    uint32_t DataStart() const { return PAGE_SIZE; }

    // Returns the exclusive end of the file data area (the full flash region).
    uint32_t DataEnd() const
    {
        return STORAGE_FLASH_SIZE;
    }

    // Reads a single table entry from the current table.
    bool ReadTableEntry(uint32_t idx, FileRecord *entry)
    {
        if (file_table_offset == 0 || idx >= TableCapacity())
            return false;
        return Storage_FlashRead(file_table_offset + idx * TABLE_ENTRY_SIZE, entry, TABLE_ENTRY_SIZE) == TABLE_ENTRY_SIZE;
    }

    // True when any valid file record (or the pending reservation) covers the page at `offset`.
    bool BlockUsed(uint32_t offset)
    {
        if (file_table_offset == 0)
            return RangeIsPending(offset);
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 0; i < capacity; i++) {
            FileRecord entry;
            if (!ReadTableEntry(i, &entry))
                return true;
            if (FileSlotIsFree(entry.offset)) continue;
            uint32_t start = entry.offset;
            // A file always occupies at least one block, even when its size is 0:
            // CreateFile/ResizeFile reserve (and erase) one block for a zero-size file,
            // so BlockUsed must report it as used. Without this clamp a size-0 file's
            // block looks free and FindSpace hands it to another file -> two live records
            // alias the same flash (observed on hardware: ZERO@512 + BETA@512, and after
            // resizing ZERO up, ZERO shadowed BETA's data region).
            uint32_t blocks = BlocksForSize(entry.size);
            if (blocks == 0) blocks = 1;
            uint32_t end = entry.offset + blocks * PAGE_SIZE;
            if (offset >= start && offset < end)
                return true;
        }
        return RangeIsPending(offset);
    }

    // Validates the current table (entry 0 must point to itself with correct size) and
    // derives the table length. Reads entry 0 directly: file_table_size is not known yet.
    bool ValidateTable()
    {
        if (file_table_offset == 0) return false;

        FileRecord entry0;
        if (Storage_FlashRead(file_table_offset, &entry0, TABLE_ENTRY_SIZE) != TABLE_ENTRY_SIZE)
            return false;

        if (!FileRecordIsValid(entry0.offset) ||
            entry0.offset != file_table_offset ||
            entry0.size < PAGE_SIZE ||
            (entry0.size % PAGE_SIZE) != 0 ||
            file_table_offset > DataEnd() ||
            entry0.size > DataEnd() - file_table_offset)
            return false;

        file_table_size = entry0.size;
        return true;
    }

    uint32_t wear_cursor = 0;  // Rotating allocation cursor for even wear (in data pages)
} Storage;
