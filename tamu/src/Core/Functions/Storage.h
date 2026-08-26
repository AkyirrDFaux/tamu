#pragma once

#include <cstring>
#include <cstdint>
#include "Core/Functions/Log.h"

// Total storage flash available to the file layer. Constant at compile time: the device
// build flags define it (see platformio.ini), so the directory/data layout never depends on
// runtime values and no RAM is spent caching flash geometry.
#ifndef STORAGE_FLASH_SIZE
#define STORAGE_FLASH_SIZE 0x10000
#endif
// Filesystem block/page size: allocation granularity and erase unit. Defined per device
// (4096 on ESP32-based devices; 64 on the CH32V003 DAS node, matching its hardware erase
// page).
#ifndef STORAGE_BLOCK_SIZE
#define STORAGE_BLOCK_SIZE 4096
#endif
// Number of concurrent write streams. Default 4; RAM-starved devices (DAS) build with a
// smaller value via the STORAGE_MAX_STREAMS build flag.
#ifndef STORAGE_MAX_STREAMS
#define STORAGE_MAX_STREAMS 4
#endif
#define MAX_STREAMS STORAGE_MAX_STREAMS
#define PAGE_SIZE STORAGE_BLOCK_SIZE

// Upper bound on allocatable data pages (worst-case flash geometry); sizes the per-call
// usage bitmap in FindSpace (STORAGE_FLASH_SIZE / PAGE_SIZE bits).
#define STORAGE_MAX_BLOCKS ((STORAGE_FLASH_SIZE / PAGE_SIZE) + 1)

// --- Flash access (implemented per device, see Devices/<device>/Storage.h) ---
bool Storage_FlashInit();                              // find/open the storage partition
uint32_t Storage_FlashSize();                          // total flash bytes available
uint32_t Storage_FlashRead(uint32_t offset, void *data, uint32_t size);   // Reads `size` bytes from flash at `offset`; returns bytes actually read (0 on failure)
bool Storage_FlashWrite(uint32_t offset, const void *data, uint32_t size); // Writes `size` bytes to flash at `offset`
bool Storage_FlashErase(uint32_t offset, uint32_t size);   // Erases `size` bytes of flash starting at `offset`
bool Storage_FlashFormat();                            // Wipes the entire storage region (per-device)

// File record format (16 bytes, naturally 4-aligned, Docs/Services/Storage.md):
// Offset is from flash start, Filesize in bytes, Name is 8 plain-text characters.
struct FileEntry
{
    uint32_t offset;    // 0x00 = invalidated entry, 0xFFFFFFFF = unwritten slot
    uint32_t size;
    char name[8];
};

#define TABLE_ENTRY_SIZE sizeof(FileEntry)

// Number of whole pages a byte size occupies, clamped to [1, MAX_DATA_BLOCKS].
// Plain `(size + PAGE_SIZE - 1) / PAGE_SIZE` overflows uint32 for sizes near
// 0xFFFFFFFF (the Storage service passes untrusted sizes straight in), silently
// yielding a tiny block count for a huge file.
static inline uint32_t BlocksForSize(uint32_t size)
{
    if (size > (0xFFFFFFFFu - PAGE_SIZE + 1))
        return (0xFFFFFFFFu / PAGE_SIZE) + 1; // saturate: no overflow
    uint32_t blocks = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (blocks == 0) blocks = 1;
    if (blocks > STORAGE_MAX_BLOCKS) blocks = STORAGE_MAX_BLOCKS;
    return blocks;
}

// Number of 32-bit pointer slots in the first (pointer) page
#define PTR_SLOTS (PAGE_SIZE / 4)

// Write stream (CID 64+); name is 8 plain-text characters. The file's offset/size are
// cached at stream open so every write packet does not have to re-walk the file table.
struct WriteStream
{
    uint8_t cid;
    char name[8];
    uint32_t current_offset;
    uint32_t file_offset;
    uint32_t file_size;
    bool active;
};

// File-table slot state (Docs/Services/Storage.md): offset 0x00 = invalidated entry,
// 0xFFFFFFFF = unused slot, anything else = a real (page-aligned) flash offset.
static inline bool FileSlotIsFree(uint32_t offset)
{
    return offset == 0x00 || offset == 0xFFFFFFFF;
}
static inline bool FileEntryIsValid(uint32_t offset)
{
    return !FileSlotIsFree(offset);
}

// True when `name` is the reserved table self-entry (entry 0, ".TABLE").
static inline bool IsTableSelfName(const char name[8])
{
    static constexpr char table_name[8] = {'.', 'T', 'A', 'B', 'L', 'E', ' ', ' '};
    return memcmp(name, table_name, 8) == 0;
}

// Copies a plain C string into the 8-byte record form, space-padded to the full width so
// names compare equal with an exact 8-byte memcmp regardless of how they were supplied.
static inline void PackName(const char *plain, char out[8])
{
    uint8_t i = 0;
    for (; i < 8 && plain && plain[i]; i++)
        out[i] = plain[i];
    for (; i < 8; i++)
        out[i] = ' ';
}

class StorageSystem
{
public:
    WriteStream streams[MAX_STREAMS];
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

        for (int i = 0; i < MAX_STREAMS; i++)
            streams[i].active = false;

        file_table_offset = FindFiletable();
        if (file_table_offset == 0 || !ValidateTable())
            Format();
    }

    // Returns the current file table pointer from the first page. Slots are scanned one at a
    // time so a large pointer page (4096 B on the Tamu) never needs a matching stack buffer.
    // Returns the LAST valid slot, not the first: WriteTablePointer appends the new pointer
    // before invalidating the older ones, so after an interrupted update both can be valid
    // and the newest (highest slot) must win - otherwise a crash would silently boot the
    // stale table while FindSpace considers the new table's pages free.
    uint32_t FindFiletable()
    {
        uint32_t slot = 0;
        uint32_t newest = 0;
        for (uint32_t i = 0; i < PTR_SLOTS; i++) {
            if (Storage_FlashRead(i * 4, &slot, sizeof(slot)) != sizeof(slot))
                return newest;
            // Track each valid slot (neither 0x00000000 nor 0xFFFFFFFF); later slots are newer.
            if (slot != 0x00000000 && slot != 0xFFFFFFFF)
                newest = slot;
        }
        return newest;
    }

    // Updates the pointer in the first page to point to a new table. The block is erased
    // only when the last slot is used (low-wear first page).
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

        // Write new offset to the found slot
        if (!Storage_FlashWrite(write_slot * 4, &new_offset, sizeof(uint32_t)))
            return false;

        // Invalidate all earlier slots (set to 0x00000000)
        uint32_t zero = 0x00000000;
        for (uint32_t i = 0; i < write_slot; i++) {
            if (!Storage_FlashWrite(i * 4, &zero, sizeof(uint32_t)))
                return false;
        }
        return true;
    }

    // Returns the index of the file record matching `name`, or 0xFFFFFFFF if none.
    uint32_t FindInFiletable(const char name[8])
    {
        if (file_table_offset == 0) return 0xFFFFFFFF;
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 0; i < capacity; i++) {
            FileEntry entry;
            if (!ReadTableEntry(i, &entry))
                return 0xFFFFFFFF;
            if (FileEntryIsValid(entry.offset) && memcmp(entry.name, name, 8) == 0)
                return i;
        }
        return 0xFFFFFFFF;
    }

    // Returns the first non-written file record index (first slot with offset 0xFFFFFFFF).
    // Invalidated (0x00) holes are never reused, so this is the append position.
    uint32_t GetEndOfFiletable()
    {
        if (file_table_offset == 0) return 0;
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 0; i < capacity; i++) {
            FileEntry entry;
            if (!ReadTableEntry(i, &entry))
                return capacity;
            if (entry.offset == 0xFFFFFFFF)
                return i;
        }
        return capacity;
    }

    // Writes a new file record at the end of the table. If no space is available, the table
    // is filtered and moved (MoveFiletable), then the write is retried.
    bool WriteFilerecord(const FileEntry &new_record)
    {
        uint32_t slot = GetEndOfFiletable();
        if (slot >= TableCapacity()) {
            if (!MoveFiletable())
                return false;
            slot = GetEndOfFiletable();
            if (slot >= TableCapacity())
                return false;
        }
        return Storage_FlashWrite(file_table_offset + slot * TABLE_ENTRY_SIZE,
                                  &new_record, TABLE_ENTRY_SIZE);
    }

    // Invalidates the file record matching `name` in place; true if it existed.
    bool DeleteFilerecord(const char name[8])
    {
        uint32_t idx = FindInFiletable(name);
        if (idx == 0xFFFFFFFF) return true; // Already gone
        if (idx == 0) return false;         // Entry 0 (the table itself) cannot be deleted

        uint32_t zero = 0x00000000;
        return Storage_FlashWrite(file_table_offset + idx * TABLE_ENTRY_SIZE, &zero, sizeof(zero));
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
        uint32_t src = FindInFiletable(old_name);
        if (src == 0xFFFFFFFF || src == 0) return false; // missing / the table itself

        FileEntry entry;
        if (!ReadTableEntry(src, &entry)) return false;

        FileEntry rec = entry;
        memcpy(rec.name, new_name, 8);
        if (!WriteFilerecord(rec))
            return false;

        // The appended record sits at the current end; invalidate every OTHER valid
        // record still carrying either name (the previous generation under each).
        uint32_t appended = GetEndOfFiletable() - 1;
        uint32_t capacity = TableCapacity();
        uint32_t zero = 0x00000000;
        for (uint32_t i = 0; i < capacity; i++) {
            if (i == appended) continue;
            FileEntry e;
            if (!ReadTableEntry(i, &e))
                break;
            if (FileSlotIsFree(e.offset)) continue;
            if (memcmp(e.name, old_name, 8) == 0 || memcmp(e.name, new_name, 8) == 0) {
                if (!Storage_FlashWrite(file_table_offset + i * TABLE_ENTRY_SIZE,
                                        &zero, sizeof(zero)))
                    return false;
            }
        }
        return true;
    }

    // Counts valid entries; keeps the table between 25% and 75% full by growing or
    // shrinking one page per move (one page minimum). Finds a new space, writes a
    // self-describing entry 0 and copies the valid entries, then updates the first-page
    // pointer and invalidates the old pointer slot.
    bool MoveFiletable()
    {
        uint32_t capacity = TableCapacity();
        if (capacity == 0) return false;

        uint32_t valid_count = 0;
        for (uint32_t i = 0; i < capacity; i++) {
            FileEntry entry;
            if (!ReadTableEntry(i, &entry))
                return false;
            if (FileEntryIsValid(entry.offset))
                valid_count++;
        }

        uint32_t new_size = file_table_size;
        if (valid_count * 4 > capacity * 3) // > 75% full: grow by one page
            new_size += PAGE_SIZE;
        else if (valid_count * 4 < capacity && new_size > PAGE_SIZE) // < 25% full: shrink
            new_size -= PAGE_SIZE;

        uint32_t new_offset = FindSpace(new_size);
        if (new_offset == 0) return false; // Out of space

        if (!Storage_FlashErase(new_offset, new_size))
            return false;

        // Entry 0: self-describing table (points to its own location and length)
        FileEntry entry0;
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
            FileEntry entry;
            if (!ReadTableEntry(i, &entry))
                return false;
            if (!FileEntryIsValid(entry.offset)) continue;
            if (dest * TABLE_ENTRY_SIZE >= new_size)
                return false;
            if (!Storage_FlashWrite(new_offset + dest * TABLE_ENTRY_SIZE, &entry, TABLE_ENTRY_SIZE))
                return false;
            dest++;
        }

        // Point at the new table and invalidate the old pointer slot.
        if (!WriteTablePointer(new_offset))
            return false;

        file_table_offset = new_offset;
        file_table_size = new_size;
        DeviceLog("STORAGE", "Table moved to 0x%x (%d B, %d valid)", (unsigned)new_offset,
                  (int)new_size, (int)valid_count);
        return true;
    }

    // Finds contiguous free space of `size` bytes starting at the best-fitting page, excluding
    // existing files and the pointer page. A rotating cursor spreads wear across the storage.
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

        // Build the usage bitmap once: BlockUsed() rescans the whole file table per
        // block, so the previous per-candidate probing was O(blocks x table) flash reads.
        uint8_t used_bitmap[(STORAGE_MAX_BLOCKS + 7) / 8] = {0};
        for (uint32_t i = 0; i < num_blocks; i++)
        {
            if (BlockUsed(data_start + i * PAGE_SIZE) || RangeIsPending(data_start + i * PAGE_SIZE))
                used_bitmap[i >> 3] |= (uint8_t)(1u << (i & 7));
        }

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
    }

    // Creates a file of `size` bytes; returns true on success.
    bool CreateFile(const char name[8], uint32_t size)
    {
        if (FindInFiletable(name) != 0xFFFFFFFF)
            return false;

        uint32_t data_offset = FindSpace(size);
        if (data_offset == 0) return false; // Out of space

        uint32_t data_blocks = BlocksForSize(size);
        if (!Storage_FlashErase(data_offset, data_blocks * PAGE_SIZE))
            return false;

        FileEntry new_record;
        new_record.offset = data_offset;
        new_record.size = size;
        memcpy(new_record.name, name, 8);

        // The committing record is not in the table yet. Reserve the area so a table
        // move triggered by WriteFilerecord can never relocate the file table onto
        // this freshly erased (and invisible) space.
        pending_offset = data_offset;
        pending_blocks = data_blocks;
        bool ok = WriteFilerecord(new_record);
        pending_offset = 0;
        pending_blocks = 0;
        return ok;
    }

    // Invalidates the file table entry for `name`; true if it existed (or was already gone).
    bool DeleteFile(const char name[8])
    {
        return DeleteFilerecord(name);
    }

    // Grows or shrinks a file to `new_size`. Shrinking always succeeds in place; growing
    // extends in place when possible, otherwise copies only when `copy_if_failed` is set.
    bool ResizeFile(const char name[8], uint32_t new_size, bool copy_if_failed = false)
    {
        uint32_t idx = FindInFiletable(name);
        if (idx == 0xFFFFFFFF) return false;
        if (idx == 0) return false; // Entry 0 (the table itself) cannot be resized

        FileEntry entry;
        if (!ReadTableEntry(idx, &entry))
            return false;

        uint32_t current_blocks = BlocksForSize(entry.size);
        uint32_t new_blocks = BlocksForSize(new_size);

        if (new_blocks <= current_blocks) {
            // Shrink or same size: same offset, new size record, invalidate old one.
            FileEntry new_record = entry;
            new_record.size = new_size;
            if (!WriteFilerecord(new_record))
                return false;
            DeleteFilerecord(name);
            return true;
        }

        // Enlargement: check if the run can be extended in place.
        bool can_extend = true;
        for (uint32_t b = 0; b < new_blocks; b++) {
            if (b >= current_blocks && BlockUsed(entry.offset + b * PAGE_SIZE)) {
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
            FileEntry new_record = entry;
            new_record.size = new_size;
            // Reserve the erased tail so a table move triggered by WriteFilerecord
            // cannot relocate the table onto it (same rule as CreateFile).
            pending_offset = tail_offset;
            pending_blocks = tail_blocks;
            bool ok = WriteFilerecord(new_record);
            pending_offset = 0;
            pending_blocks = 0;
            if (!ok) return false;
            DeleteFilerecord(name);
            return true;
        }

        // Extending in place is not possible: copy only if requested.
        if (!copy_if_failed)
            return false;

        uint32_t new_offset = FindSpace(new_size);
        if (new_offset == 0) return false;
        if (!Storage_FlashErase(new_offset, new_blocks * PAGE_SIZE))
            return false;

        // Reserve the destination while the committing record is still unwritten, so a
        // table move inside WriteFilerecord cannot land on this erased area.
        pending_offset = new_offset;
        pending_blocks = new_blocks;

        uint8_t chunk[64];
        uint32_t remaining = entry.size;
        uint32_t source = entry.offset;
        uint32_t destination = new_offset;
        bool copy_ok = true;
        while (remaining > 0) {
            uint32_t chunk_size = (remaining > sizeof(chunk)) ? sizeof(chunk) : remaining;
            if (Storage_FlashRead(source, chunk, chunk_size) != chunk_size) { copy_ok = false; break; }
            if (!Storage_FlashWrite(destination, chunk, chunk_size)) { copy_ok = false; break; }
            source += chunk_size;
            destination += chunk_size;
            remaining -= chunk_size;
        }

        bool ok = false;
        if (copy_ok)
        {
            FileEntry new_record = entry;
            new_record.offset = new_offset;
            new_record.size = new_size;
            ok = WriteFilerecord(new_record);
        }

        pending_offset = 0;
        pending_blocks = 0;

        if (!ok)
            return false;
        DeleteFilerecord(name);
        return true;
    }

    // Utility wrapper for reading from a file; offset is from file start. Returns bytes read.
    uint32_t ReadFromFile(const char name[8], uint32_t offset, uint32_t length, char *buffer)
    {
        uint32_t file_offset, file_size;
        if (!GetFileInfo(name, &file_offset, &file_size))
            return 0;
        if (offset >= file_size)
            return 0;
        if (offset + length > file_size)
            length = file_size - offset;
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
        if (offset + length > file_size)
            length = file_size - offset;
        return Storage_FlashWrite(file_offset + offset, buffer, length);
    }

    // Returns the file size if it exists, otherwise 0xFFFFFFFF.
    uint32_t FileExists(const char name[8])
    {
        uint32_t idx = FindInFiletable(name);
        if (idx == 0xFFFFFFFF)
            return 0xFFFFFFFF;
        FileEntry entry;
        if (!ReadTableEntry(idx, &entry))
            return 0xFFFFFFFF;
        return entry.size;
    }

    // Erases the pointer page and storage, then initializes an empty self-describing table
    // of one page at the first data page (block 1).
    void Format()
    {
        if (!Storage_FlashFormat()) {
            DeviceLog("STORAGE", "Format failed!");
            return;
        }

        FileEntry entry0;
        memset(&entry0, 0xFF, sizeof(entry0));
        entry0.offset = PAGE_SIZE;              // Table starts at block 1
        entry0.size = PAGE_SIZE;                // One page of table capacity
        memcpy(entry0.name, ".TABLE  ", 8);

        if (Storage_FlashWrite(PAGE_SIZE, &entry0, TABLE_ENTRY_SIZE) != true)
            return;

        if (!WriteTablePointer(PAGE_SIZE))
            return;

        file_table_offset = PAGE_SIZE;
        file_table_size = PAGE_SIZE;
        wear_cursor = 0;
        DeviceLog("STORAGE", "Formatted, storage ready (%d bytes)", (int)DataEnd());
    }

    // Returns the number of valid files in the file table (including entry 0).
    uint8_t FileCount()
    {
        if (file_table_offset == 0) return 0;
        uint8_t count = 0;
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 0; i < capacity; i++) {
            FileEntry entry;
            if (!ReadTableEntry(i, &entry)) continue;
            if (FileEntryIsValid(entry.offset)) count++;
        }
        return count;
    }

    // Copies the `idx`-th valid file entry (dense across valid files) into `out`.
    bool ReadFileEntry(uint8_t idx, FileEntry *out)
    {
        if (file_table_offset == 0) return false;
        uint8_t seen = 0;
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 0; i < capacity; i++) {
            FileEntry entry;
            if (!ReadTableEntry(i, &entry))
                return false;
            if (FileSlotIsFree(entry.offset)) continue;
            if (seen == idx) {
                *out = entry;
                return true;
            }
            seen++;
        }
        return false;
    }

    // Looks up a file by its name; returns offset/size via the out params.
    bool GetFileInfo(const char name[8], uint32_t *offset, uint32_t *size)
    {
        uint32_t idx = FindInFiletable(name);
        if (idx == 0xFFFFFFFF) return false;
        FileEntry entry;
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
    bool ReadTableEntry(uint32_t idx, FileEntry *entry)
    {
        if (file_table_offset == 0 || idx >= TableCapacity())
            return false;
        return Storage_FlashRead(file_table_offset + idx * TABLE_ENTRY_SIZE, entry, TABLE_ENTRY_SIZE) == TABLE_ENTRY_SIZE;
    }

    // True when any valid file entry (or the pending reservation) covers the page at `offset`.
    bool BlockUsed(uint32_t offset)
    {
        if (file_table_offset == 0)
            return RangeIsPending(offset);
        uint32_t capacity = TableCapacity();
        for (uint32_t i = 0; i < capacity; i++) {
            FileEntry entry;
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

        FileEntry entry0;
        if (Storage_FlashRead(file_table_offset, &entry0, TABLE_ENTRY_SIZE) != TABLE_ENTRY_SIZE)
            return false;

        if (!FileEntryIsValid(entry0.offset) ||
            entry0.offset != file_table_offset ||
            entry0.size < PAGE_SIZE ||
            (entry0.size % PAGE_SIZE) != 0 ||
            file_table_offset + entry0.size > DataEnd())
            return false;

        file_table_size = entry0.size;
        return true;
    }

    uint32_t wear_cursor;  // Rotating allocation cursor for even wear (in data pages)
} Storage;

// Allocates a write stream (CID 64+) for an existing file at `offset` and caches the file
// location. Returns the stream CID (64+) or 0 when the file is missing or no slot is free.
static inline uint8_t StorageStreamOpen(const char name[8], uint32_t offset)
{
    uint32_t file_offset = 0, file_size = 0;
    if (!Storage.GetFileInfo(name, &file_offset, &file_size))
        return 0;
    if (offset >= file_size)
        return 0; // a stream only fills an existing file's bounds
    for (int i = 0; i < MAX_STREAMS; i++)
    {
        if (!Storage.streams[i].active)
        {
            Storage.streams[i].active = true;
            Storage.streams[i].cid = 64 + i;
            memcpy(Storage.streams[i].name, name, 8);
            Storage.streams[i].current_offset = offset;
            Storage.streams[i].file_offset = file_offset;
            Storage.streams[i].file_size = file_size;
            return Storage.streams[i].cid;
        }
    }
    return 0;
}

// Writes `len` payload bytes to the stream `stream_idx` (CID 64+). Chunks are clamped to
// the cached file size so a stream can never overwrite flash beyond the file.
static inline void StorageStreamWrite(uint8_t stream_idx, const uint8_t *payload, uint8_t len)
{
    if (stream_idx >= MAX_STREAMS || !Storage.streams[stream_idx].active)
    {
        DeviceLog("STORAGE", "write to inactive stream %u", (unsigned)stream_idx);
        return;
    }
    WriteStream &stream = Storage.streams[stream_idx];
    uint32_t file_offset = stream.file_offset;
    uint32_t file_size = stream.file_size;
    if (stream.current_offset < file_size)
    {
        uint32_t chunk_len = len;
        if (stream.current_offset + chunk_len > file_size)
            chunk_len = file_size - stream.current_offset;
        if (!Storage_FlashWrite(file_offset + stream.current_offset, payload, chunk_len))
            DeviceLog("STORAGE", "stream %u flash write failed at %u", (unsigned)stream_idx,
                      (unsigned)stream.current_offset);
        stream.current_offset += chunk_len;
        if (stream.current_offset >= file_size)
            stream.active = false; // file complete: stop accepting packets
    }
    else
    {
        DeviceLog("STORAGE", "stream %u write past EOF (offset %u >= size %u)",
                  (unsigned)stream_idx, (unsigned)stream.current_offset, (unsigned)file_size);
    }
}

// Closes the stream with CID `cid` (64+), if active.
static inline void StorageStreamClose(uint8_t cid)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (Storage.streams[i].active && Storage.streams[i].cid == cid)
            Storage.streams[i].active = false;
}