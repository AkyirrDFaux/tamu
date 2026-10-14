#pragma once

// The full multi-file file system (Tamu).
//
// Part of Core/Functions/Storage.h (included from there).

#include "Core/Functions/StorageDefs.h"

class StorageSystem
{
public:
    uint32_t FileTableOffset;  // Offset of the current file table (0 if none)
    uint32_t FileTableSize;    // Current file table size in bytes (multiple of STORAGE_BLOCK_SIZE)

    // Initializes flash access, recovers the file table and formats if none is valid.
    void Init()
    {
        if (!StorageFlashInit()) {
            DeviceLog("STORAGE", "Storage flash not available!");
            FileTableOffset = 0;
            FileTableSize = 0;
            return;
        }

        // A read that FAILS is not the same as a table that is absent, and only the second may be
        // answered with a format: erasing on a transient read error would destroy every saved file.
        // Retry once, then leave storage disabled for this session rather than wipe the partition.
        if (!FindFiletable(&FileTableOffset) && !FindFiletable(&FileTableOffset)) {
            DeviceLog("STORAGE", "table pointer unreadable - NOT formatting");
            FileTableOffset = 0;
            FileTableSize = 0;
            return;
        }
        if (FileTableOffset == 0) {
            // No table has ever been written: formatting is correct and creates one.
            Format();
            return;
        }
        if (!ValidateTable()) {
            // The pointer read, so the table it names is present but does not validate. Separate a
            // failed read of entry 0 - which says nothing about whether the files are there - from
            // a genuinely invalid table before erasing anything.
            FileRecord Entry0;
            if (StorageFlashRead(FileTableOffset, &Entry0, TABLE_ENTRY_SIZE) != TABLE_ENTRY_SIZE) {
                DeviceLog("STORAGE", "table entry 0 unreadable - NOT formatting");
                FileTableOffset = 0;
                FileTableSize = 0;
                return;
            }
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
    bool FindFiletable(uint32_t *Offset)
    {
        uint32_t Slot = 0;
        uint32_t Newest = 0;
        for (uint32_t i = 0; i < PTR_SLOTS; i++) {
            if (StorageFlashRead(i * 4, &Slot, sizeof(Slot)) != sizeof(Slot))
                return false;
            // Track each valid slot (neither 0x00000000 nor 0xFFFFFFFF); later slots are newer.
            if (Slot != 0x00000000 && Slot != 0xFFFFFFFF)
                Newest = Slot;
        }
        *Offset = Newest;
        return true;
    }

    // Appends a new table pointer to the first page (erasing it only when the last slot is
    // used; low-wear first page). Older slots are left untouched - the newest valid slot wins
    // on read (B13), so invalidating them is only unnecessary flash writes.
    bool WriteTablePointer(uint32_t NewOffset)
    {
        uint32_t Slot = 0;
        uint32_t WriteSlot = PTR_SLOTS;
        for (uint32_t i = 0; i < PTR_SLOTS; i++) {
            if (StorageFlashRead(i * 4, &Slot, sizeof(Slot)) != sizeof(Slot))
                return false;
            if (Slot == 0xFFFFFFFF) { // first unused slot
                WriteSlot = i;
                break;
            }
        }

        if (WriteSlot == PTR_SLOTS) {
            // Block is full, erase it and start fresh (erased flash is 0xFFFFFFFF = "unused").
            if (!StorageFlashErase(0, STORAGE_BLOCK_SIZE))
                return false;
            return StorageFlashWrite(0, &NewOffset, sizeof(uint32_t));
        }

        // Write new offset to the found slot; older slots are left untouched (newest wins).
        return StorageFlashWrite(WriteSlot * 4, &NewOffset, sizeof(uint32_t));
    }

    // Finds the NEWEST file record matching `name`. Returns false when a flash read fails
    // (the table is partially unreadable, so no result is trustworthy); on success `*index`
    // is the record index, or 0xFFFFFFFF when none. The newest (highest slot) wins so a
    // duplicate left by an older build cannot shadow the current file (RenameFile appends
    // the replacement).
    bool FindInFiletable(const char Name[8], uint32_t *Index)
    {
        if (FileTableOffset == 0) { *Index = 0xFFFFFFFF; return true; }
        uint32_t Capacity = TableCapacity();
        uint32_t Found = 0xFFFFFFFF;
        for (uint32_t i = 0; i < Capacity; i++) {
            FileRecord Entry;
            if (!ReadTableEntry(i, &Entry))
                return false;
            if (FileRecordIsValid(Entry.Offset) && NameMatch(Entry.Name, Name))
                Found = i;
        }
        *Index = Found;
        return true;
    }

    // Finds the first non-written file record index (first slot with offset 0xFFFFFFFF).
    // Returns false when a flash read fails; on success `*index` is that slot, or the
    // capacity when the table is full. Invalidated (0x00) holes are never reused, so this
    // is the append position.
    bool GetEndOfFiletable(uint32_t *Index)
    {
        if (FileTableOffset == 0) { *Index = 0; return true; }
        uint32_t Capacity = TableCapacity();
        for (uint32_t i = 0; i < Capacity; i++) {
            FileRecord Entry;
            if (!ReadTableEntry(i, &Entry))
                return false;
            if (Entry.Offset == 0xFFFFFFFF) { *Index = i; return true; }
        }
        *Index = Capacity;
        return true;
    }

    // Writes a new file record at the end of the table. If no space is available, the table
    // is filtered and moved (MoveFiletable), then the write is retried.
    bool WriteFileRecord(const FileRecord &NewRecord)
    {
        uint32_t Slot;
        if (!GetEndOfFiletable(&Slot))
            return false;
        if (Slot >= TableCapacity()) {
            if (!MoveFiletable())
                return false;
            if (!GetEndOfFiletable(&Slot))
                return false;
            if (Slot >= TableCapacity())
                return false;
        }
        return StorageFlashWrite(FileTableOffset + Slot * TABLE_ENTRY_SIZE,
                                  &NewRecord, TABLE_ENTRY_SIZE);
    }

    // Invalidates EVERY file record matching `name` in place by zeroing the offset word
    // only (the same 4-byte policy as every other invalidation path and as the app's
    // readFileTable, which skips any record whose offset is 0 on the full file system;
    // the size is left in place). All matches are removed so pre-existing duplicates are
    // fully deleted, not just the newest one.
    template <typename Match>
    bool DeleteMatching(Match Matches)
    {
        if (FileTableOffset == 0) return true;
        uint32_t Capacity = TableCapacity();
        for (uint32_t i = 1; i < Capacity; i++) { // entry 0 (the table) is never deleted
            FileRecord Entry;
            if (!ReadTableEntry(i, &Entry))
                return false;
            if (FileSlotIsFree(Entry.Offset)) continue;
            if (!Matches(Entry.Name)) continue;
            uint32_t Zero = 0x00000000;
            if (!StorageFlashWrite(FileTableOffset + i * TABLE_ENTRY_SIZE, &Zero, sizeof(Zero)))
                return false;
        }
        return true; // already gone is fine
    }

    bool DeleteFileRecord(const char Name[8])
    {
        return DeleteMatching([&](const char *EntryName) { return NameMatch(EntryName, Name); });
    }

    // Invalidates every live record whose name satisfies `matches`, except the freshly appended
    // generation at `keep_index`. Zeroes the offset word only (as every invalidation path does),
    // so a replaced generation is never mistaken for the current file.
    template <typename Match>
    bool InvalidateMatchingExcept(Match Matches, uint32_t KeepIndex)
    {
        if (FileTableOffset == 0) return true;
        uint32_t Capacity = TableCapacity();
        uint32_t Zero = 0x00000000;
        for (uint32_t i = 0; i < Capacity; i++) {
            if (i == KeepIndex) continue;
            FileRecord Entry;
            if (!ReadTableEntry(i, &Entry))
                return false;
            if (FileSlotIsFree(Entry.Offset)) continue;
            if (!Matches(Entry.Name)) continue;
            if (!StorageFlashWrite(FileTableOffset + i * TABLE_ENTRY_SIZE,
                                    &Zero, sizeof(Zero)))
                return false;
        }
        return true;
    }

    // Calls `fn(name)` once for every live file record. A superseded record for the same
    // name can linger until the next save, so a name may repeat; callers that care dedupe.
    template <typename Func>
    // Returns true only when the whole table was read. It reads the table lazily by index, so an
    // early return is a PARTIAL walk, not an empty table: callers that delete on absence must check
    // this, or a failed read looks exactly like "nothing is there".
    bool ForEachFile(Func Fn)
    {
        if (FileTableOffset == 0) return false;
        uint32_t Capacity = TableCapacity();
        for (uint32_t i = 1; i < Capacity; i++) { // entry 0 (the table) is not a file
            FileRecord Entry;
            if (!ReadTableEntry(i, &Entry)) return false;
            if (FileSlotIsFree(Entry.Offset)) continue;
            Fn(Entry.Name);
        }
        return true;
    }

    // Renames `old_name` to `new_name`: appends a record with the new name for the same
    // data area, then invalidates the superseded records (Docs/Services/Storage.md).
    //
    // Crash-safe by construction - each step is an append-only NOR write or a 1->0
    // record invalidation, so a power cut leaves readers resolving either the old or the
    // new name to a complete file. An existing `new_name` is replaced (its previous data
    // area becomes unreferenced and therefore free for future allocation).
    bool RenameFile(const char OldName[8], const char NewName[8])
    {
        uint32_t Src;
        if (!FindInFiletable(OldName, &Src) || Src == 0xFFFFFFFF || Src == 0)
            return false; // read failure / missing / the table itself

        FileRecord Entry;
        if (!ReadTableEntry(Src, &Entry)) return false;

        FileRecord Rec = Entry;
        // Space-pad the new name: the caller passes a plain C string and a raw
        // memcpy would copy its NUL terminator into the record (breaking later
        // 8-byte memcmp lookups and the app's file-type detection).
        PackName(NewName, Rec.Name);
        if (!WriteFileRecord(Rec))
            return false;

        // The appended record sits at the current end; invalidate every OTHER valid
        // record still carrying either name (the previous generation under each). If the
        // end cannot be re-read the new record stays live and the old one is untouched
        // (no data loss); report the failure.
        uint32_t Appended;
        if (!GetEndOfFiletable(&Appended) || Appended == 0)
            return false;
        return InvalidateMatchingExcept(
            [&](const char *EntryName) { return NameMatch(EntryName, OldName) || NameMatch(EntryName, NewName); },
            Appended - 1);
    }

    // Moves the full table into a fresh area of the same, fixed size (B15). Finds a new
    // space, erases it, writes a self-describing entry 0 and copies the valid entries
    // (compacting out the holes), then appends the new first-page pointer.
    bool MoveFiletable()
    {
        uint32_t Capacity = TableCapacity();
        if (Capacity == 0) return false;

        uint32_t NewSize = FileTableSize; // fixed per device (STORAGE_TABLE_SIZE)

        uint32_t NewOffset = FindSpace(NewSize);
        if (NewOffset == 0) return false; // Out of space

        if (!StorageFlashErase(NewOffset, NewSize))
            return false;

        // Entry 0: self-describing table (points to its own location and length)
        FileRecord Entry0;
        memset(&Entry0, 0xFF, sizeof(Entry0));
        Entry0.Offset = NewOffset;
        Entry0.Size = NewSize;
        memcpy(Entry0.Name, ".TABLE  ", 8);
        if (!StorageFlashWrite(NewOffset, &Entry0, TABLE_ENTRY_SIZE))
            return false;

        // Copy valid entries (dense, skipping holes); entry 0 is regenerated above.
        uint32_t Dest = 1;
        for (uint32_t i = 0; i < Capacity; i++) {
            if (i == 0) continue;
            FileRecord Entry;
            if (!ReadTableEntry(i, &Entry))
                return false;
            if (!FileRecordIsValid(Entry.Offset)) continue;
            if (Dest * TABLE_ENTRY_SIZE >= NewSize)
                return false;
            if (!StorageFlashWrite(NewOffset + Dest * TABLE_ENTRY_SIZE, &Entry, TABLE_ENTRY_SIZE))
                return false;
            Dest++;
        }

        // Point at the new table (the old pointer slot is left in place; newest wins).
        if (!WriteTablePointer(NewOffset))
            return false;

        FileTableOffset = NewOffset;
        FileTableSize = NewSize;
        DeviceLog("STORAGE", "Table moved to 0x%x (%d B)", (unsigned)NewOffset,
                  (int)NewSize);
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
    uint32_t FindSpace(uint32_t SizeBytes)
    {
        uint32_t Blocks = BlocksForSize(SizeBytes);

        uint32_t DataAreaStart = DataStart();
        uint32_t DataAreaEnd = DataEnd();
        if (DataAreaEnd <= DataAreaStart)
            return 0;
        uint32_t NumBlocks = (DataAreaEnd - DataAreaStart) / STORAGE_BLOCK_SIZE;
        if (Blocks > NumBlocks)
            return 0;

#ifdef OPTIMIZE_SPEED
        // Build the usage bitmap directly from the file table (~entries, not
        // num_blocks x entries): per-candidate BlockUsed() rescans the whole table each time.
        uint8_t UsedBitmap[(STORAGE_MAX_BLOCKS + 7) / 8] = {0};
        auto MarkUsed = [&](uint32_t Start, uint32_t Nblocks)
        {
            if (Start < DataAreaStart || Start >= DataAreaEnd) return; // outside the data area
            uint32_t First = (Start - DataAreaStart) / STORAGE_BLOCK_SIZE;
            for (uint32_t k = 0; k < Nblocks && First + k < NumBlocks; k++)
                UsedBitmap[(First + k) >> 3] |= (uint8_t)(1u << ((First + k) & 7));
        };
        for (uint32_t i = 0; i < TableCapacity(); i++)
        {
            FileRecord Entry;
            if (!ReadTableEntry(i, &Entry)) return 0; // unreadable table: no space is safe
            if (FileSlotIsFree(Entry.Offset)) continue;
            uint32_t Blocks = BlocksForSize(Entry.Size);
            if (Blocks == 0) Blocks = 1;
            MarkUsed(Entry.Offset, Blocks);
        }
        if (PendingBlocks)
            MarkUsed(PendingOffset, PendingBlocks);

        uint32_t BestOffset = 0;
        uint32_t BestRun = 0;

        // Scan free runs starting at the wear cursor (wrapping) so allocation placement
        // rotates across the storage over time, then pick the best-fit run.
        for (uint32_t Scanned = 0; Scanned < NumBlocks; ) {
            uint32_t Idx = (WearCursor + Scanned) % NumBlocks;
            uint32_t BlockOffset = DataAreaStart + Idx * STORAGE_BLOCK_SIZE;

            if (UsedBitmap[Idx >> 3] & (1u << (Idx & 7))) {
                Scanned++;
                continue;
            }

            // Measure the contiguous free run starting here. The address space is
            // LINEAR (files are a contiguous [offset, offset+blocks*PAGE) range), so a
            // run must NOT wrap past the end of the data area: a wrapped run would
            // allocate pages that physically live at the start of storage, outside the
            // file's linear range - invisible to BlockUsed (double-allocation) and
            // unreadable/unwritable through the file API.
            uint32_t Run = 0;
            while (Run < NumBlocks)
            {
                uint32_t Probe = Idx + Run;
                if (Probe >= NumBlocks) break;
                if (UsedBitmap[Probe >> 3] & (1u << (Probe & 7)))
                    break;
                Run++;
            }

            if (Run >= Blocks && (BestOffset == 0 || Run < BestRun)) {
                BestOffset = BlockOffset;
                BestRun = Run;
            }
            Scanned += Run; // skip the measured run
        }

        if (BestOffset == 0)
            return 0;

        // Advance the wear cursor past the allocated run so the next allocation starts later.
        uint32_t Idx = (BestOffset - DataAreaStart) / STORAGE_BLOCK_SIZE;
        WearCursor = (Idx + Blocks) % NumBlocks;
        return BestOffset;
#else
        // Size variant: first-fit linear scan from the wear cursor. A run must not wrap past
        // the data end (files are a linear [offset, offset+blocks*PAGE) range).
        for (uint32_t Scanned = 0; Scanned < NumBlocks; Scanned++)
        {
            uint32_t Idx = (WearCursor + Scanned) % NumBlocks;
            if (Idx + Blocks > NumBlocks)
                continue; // would wrap the linear data area
            uint32_t Run = 0;
            while (Run < Blocks && !BlockUsed(DataAreaStart + (Idx + Run) * STORAGE_BLOCK_SIZE))
                Run++;
            if (Run == Blocks)
            {
                WearCursor = (Idx + Blocks) % NumBlocks;
                return DataAreaStart + Idx * STORAGE_BLOCK_SIZE;
            }
        }
        return 0;
#endif
    }

    // Creates a file of `size` bytes; returns true on success.
    bool CreateFile(const char Name[8], uint32_t Size)
    {
        // A zero-size file cannot exist (Docs/Services/Storage.md: Create File - "Size (>0)").
        if (Size == 0) return false;

        // Refuse a duplicate (and any unreadable table: a read failure must not create a
        // second record for a name that might already exist).
        uint32_t Existing;
        if (!FindInFiletable(Name, &Existing) || Existing != 0xFFFFFFFF)
            return false;

        uint32_t DataOffset = FindSpace(Size);
        if (DataOffset == 0) return false; // Out of space

        uint32_t DataBlocks = BlocksForSize(Size);
        if (!StorageFlashErase(DataOffset, DataBlocks * STORAGE_BLOCK_SIZE))
            return false;

        FileRecord NewRecord;
        NewRecord.Offset = DataOffset;
        NewRecord.Size = Size;
        PackName(Name, NewRecord.Name);

        // The committing record is not in the table yet. Reserve the area so a table
        // move triggered by WriteFileRecord can never relocate the file table onto
        // this freshly erased (and invisible) space.
        PendingOffset = DataOffset;
        PendingBlocks = DataBlocks;
        bool Ok = WriteFileRecord(NewRecord);
        PendingOffset = 0;
        PendingBlocks = 0;
        return Ok;
    }

    // Invalidates the file table entry for `name`; true if it existed (or was already gone).
    bool DeleteFile(const char Name[8])
    {
        return DeleteFileRecord(Name);
    }

    // Grows or shrinks a file to `new_size`. Shrinking always succeeds in place; growing
    // only succeeds when the run can be extended in place (and stays inside the data area).
    bool ResizeFile(const char Name[8], uint32_t NewSize)
    {
        // A zero-size file cannot exist (Docs/Services/Storage.md: Resize File - "New Size (>0)").
        if (NewSize == 0) return false;

        uint32_t Idx;
        if (!FindInFiletable(Name, &Idx) || Idx == 0xFFFFFFFF || Idx == 0)
            return false; // read failure / missing / entry 0 (the table itself)

        FileRecord Entry;
        if (!ReadTableEntry(Idx, &Entry))
            return false;

        uint32_t CurrentBlocks = BlocksForSize(Entry.Size);
        uint32_t NewBlocks = BlocksForSize(NewSize);

        if (NewBlocks <= CurrentBlocks) {
            // Shrink or same size: append the new record, then invalidate the previous
            // generation (and any older duplicate) by index - never DeleteFileRecord, which
            // would also remove the record just appended and orphan the data.
            FileRecord NewRecord = Entry;
            NewRecord.Size = NewSize;
            if (!WriteFileRecord(NewRecord))
                return false;
            uint32_t Appended;
            if (!GetEndOfFiletable(&Appended) || Appended == 0)
                return false;
            return InvalidateMatchingExcept([&](const char *EntryName) { return NameMatch(EntryName, Name); },
                                            Appended - 1);
        }

        // Enlargement: reject before scanning if the file's linear range would leave the data
        // area. This also bounds the scan below (new_blocks can otherwise be huge).
        if (Entry.Offset >= DataEnd() || NewBlocks > (DataEnd() - Entry.Offset) / STORAGE_BLOCK_SIZE)
            return false;
        bool CanExtend = true;
        for (uint32_t BlockIdx = CurrentBlocks; BlockIdx < NewBlocks; BlockIdx++) {
            if (BlockUsed(Entry.Offset + BlockIdx * STORAGE_BLOCK_SIZE)) {
                CanExtend = false;
                break;
            }
        }
        if (CanExtend) {
            // Erase the newly-added tail pages (they are free, so erasing is safe).
            uint32_t TailOffset = Entry.Offset + CurrentBlocks * STORAGE_BLOCK_SIZE;
            uint32_t TailBlocks = NewBlocks - CurrentBlocks;
            if (!StorageFlashErase(TailOffset, TailBlocks * STORAGE_BLOCK_SIZE))
                return false;
            FileRecord NewRecord = Entry;
            NewRecord.Size = NewSize;
            // Reserve the erased tail so a table move triggered by WriteFileRecord
            // cannot relocate the table onto it (same rule as CreateFile).
            PendingOffset = TailOffset;
            PendingBlocks = TailBlocks;
            bool Ok = WriteFileRecord(NewRecord);
            PendingOffset = 0;
            PendingBlocks = 0;
            if (!Ok) return false;
            uint32_t Appended;
            if (!GetEndOfFiletable(&Appended) || Appended == 0)
                return false;
            return InvalidateMatchingExcept([&](const char *EntryName) { return NameMatch(EntryName, Name); },
                                            Appended - 1);
        }

        // Extending in place is not possible.
        return false;
    }

    // Utility wrapper for reading from a file; offset is from file start. Returns bytes read.
    uint32_t ReadFromFile(const char Name[8], uint32_t Offset, uint32_t Length, char *Buffer)
    {
        uint32_t FileOffset, FileSize;
        if (!GetFileInfo(Name, &FileOffset, &FileSize))
            return 0;
        if (Offset >= FileSize)
            return 0;
        Length = ClampFileLength(Offset, Length, FileSize);
        return StorageFlashRead(FileOffset + Offset, Buffer, Length);
    }

    // Safer wrapper for writing to a file; offset is from file start. Writes are clamped to
    // the file size. Returns true if the write succeeded.
    bool WriteToFile(const char Name[8], uint32_t Offset, uint32_t Length, const char *Buffer)
    {
        uint32_t FileOffset, FileSize;
        if (!GetFileInfo(Name, &FileOffset, &FileSize))
            return false;
        if (Offset >= FileSize)
            return false;
        Length = ClampFileLength(Offset, Length, FileSize);
        return StorageFlashWrite(FileOffset + Offset, Buffer, Length);
    }

    // Returns the file size if it exists, otherwise 0xFFFFFFFF. A read failure is also
    // reported as 0xFFFFFFFF (a caller cannot act on a size it could not read).
    uint32_t FileExists(const char Name[8])
    {
        uint32_t Idx;
        if (!FindInFiletable(Name, &Idx) || Idx == 0xFFFFFFFF)
            return 0xFFFFFFFF;
        FileRecord Entry;
        if (!ReadTableEntry(Idx, &Entry))
            return 0xFFFFFFFF;
        return Entry.Size;
    }

    // Erases the pointer page and storage, then initializes an empty self-describing table
    // of the fixed per-device size (STORAGE_TABLE_SIZE) at the first data page (block 1).
    bool Format()
    {
        if (!StorageFlashFormat()) {
            DeviceLog("STORAGE", "Format failed!");
            return false;
        }

        FileRecord Entry0;
        memset(&Entry0, 0xFF, sizeof(Entry0));
        Entry0.Offset = STORAGE_BLOCK_SIZE;              // Table starts at block 1
        Entry0.Size = STORAGE_TABLE_SIZE;       // Fixed per-device table capacity
        memcpy(Entry0.Name, ".TABLE  ", 8);

        if (StorageFlashWrite(STORAGE_BLOCK_SIZE, &Entry0, TABLE_ENTRY_SIZE) != true)
            return false;

        if (!WriteTablePointer(STORAGE_BLOCK_SIZE))
            return false;

        FileTableOffset = STORAGE_BLOCK_SIZE;
        FileTableSize = STORAGE_TABLE_SIZE;
        DeviceLog("STORAGE", "Formatted, storage ready (%d Bytes)", (int)DataEnd());
        return true;
    }

    // Returns the total number of bytes used by files in flash (excluding file table itself).
    uint32_t UsedFlashBytes()
    {
        if (FileTableOffset == 0) return 0;
        uint32_t Used = 0;
        uint32_t Capacity = TableCapacity();
        for (uint32_t i = 1; i < Capacity; i++) { // entry 0 is the table itself
            FileRecord Entry;
            if (!ReadTableEntry(i, &Entry)) continue;
            if (FileSlotIsFree(Entry.Offset)) continue;
            uint32_t Blocks = BlocksForSize(Entry.Size);
            if (Blocks == 0) Blocks = 1; // zero-size file still occupies one block
            Used += Blocks * STORAGE_BLOCK_SIZE;
        }
        return Used;
    }

    // Looks up a file by its name; returns offset/size via the out params.
    bool GetFileInfo(const char Name[8], uint32_t *Offset, uint32_t *Size)
    {
        uint32_t Idx;
        if (!FindInFiletable(Name, &Idx) || Idx == 0xFFFFFFFF) return false;
        FileRecord Entry;
        if (!ReadTableEntry(Idx, &Entry))
            return false;
        *Offset = Entry.Offset;
        *Size = Entry.Size;
        return true;
    }

    // Returns the number of records the current table can hold.
    uint32_t TableCapacity()
    {
        if (FileTableSize == 0) return 0;
        return FileTableSize / TABLE_ENTRY_SIZE;
    }

private:
    // Pending (reserved) allocation: a data area that has been erased for a new/moved
    // file but whose committing table record is not written yet. Treated as used so a
    // concurrent table move can never be placed over it.
    uint32_t PendingOffset = 0;
    uint32_t PendingBlocks = 0;

    // True when the page at `offset` overlaps the pending reservation.
    bool RangeIsPending(uint32_t Offset) const
    {
        if (PendingBlocks == 0)
            return false;
        uint32_t Start = PendingOffset;
        uint32_t End = PendingOffset + PendingBlocks * STORAGE_BLOCK_SIZE;
        return Offset >= Start && Offset < End;
    }

    // Returns the first flash offset of the file data area (after the pointer page).
    uint32_t DataStart() const { return STORAGE_BLOCK_SIZE; }

    // Returns the exclusive end of the file data area (the full flash region).
    uint32_t DataEnd() const
    {
        return STORAGE_FLASH_SIZE;
    }

    // Reads a single table entry from the current table.
    bool ReadTableEntry(uint32_t Idx, FileRecord *Entry)
    {
        if (FileTableOffset == 0 || Idx >= TableCapacity())
            return false;
        return StorageFlashRead(FileTableOffset + Idx * TABLE_ENTRY_SIZE, Entry, TABLE_ENTRY_SIZE) == TABLE_ENTRY_SIZE;
    }

    // True when any valid file record (or the pending reservation) covers the page at `offset`.
    bool BlockUsed(uint32_t Offset)
    {
        if (FileTableOffset == 0)
            return RangeIsPending(Offset);
        uint32_t Capacity = TableCapacity();
        for (uint32_t i = 0; i < Capacity; i++) {
            FileRecord Entry;
            if (!ReadTableEntry(i, &Entry))
                return true;
            if (FileSlotIsFree(Entry.Offset)) continue;
            uint32_t Start = Entry.Offset;
            // A file always occupies at least one block, even when its size is 0:
            // CreateFile/ResizeFile reserve (and erase) one block for a zero-size file,
            // so BlockUsed must report it as used. Without this clamp a size-0 file's
            // block looks free and FindSpace hands it to another file -> two live records
            // alias the same flash (observed on hardware: ZERO@512 + BETA@512, and after
            // resizing ZERO up, ZERO shadowed BETA's data region).
            uint32_t Blocks = BlocksForSize(Entry.Size);
            if (Blocks == 0) Blocks = 1;
            uint32_t End = Entry.Offset + Blocks * STORAGE_BLOCK_SIZE;
            if (Offset >= Start && Offset < End)
                return true;
        }
        return RangeIsPending(Offset);
    }

    // Validates the current table (entry 0 must point to itself with correct size) and
    // derives the table length. Reads entry 0 directly: file_table_size is not known yet.
    bool ValidateTable()
    {
        if (FileTableOffset == 0) return false;

        FileRecord Entry0;
        if (StorageFlashRead(FileTableOffset, &Entry0, TABLE_ENTRY_SIZE) != TABLE_ENTRY_SIZE)
            return false;

        if (!FileRecordIsValid(Entry0.Offset) ||
            Entry0.Offset != FileTableOffset ||
            Entry0.Size < STORAGE_BLOCK_SIZE ||
            (Entry0.Size % STORAGE_BLOCK_SIZE) != 0 ||
            FileTableOffset > DataEnd() ||
            Entry0.Size > DataEnd() - FileTableOffset)
            return false;

        FileTableSize = Entry0.Size;
        return true;
    }

    uint32_t WearCursor = 0;  // Rotating allocation cursor for even wear (in data pages)
} Storage;
