Storage holds large chunks whose size varies widely, from a few hundred bytes to about 1 MB. It sits on NOR flash, where an erased bit reads 1 and a written bit reads 0, and access can be slow. It is implemented per device, behind a common interface.
### File System
Aligned with pages, the smallest erasable unit.

The first page, kept for low wear, holds only a pointer to the start of the file table. Its last valid entry is that pointer, and unused entries read `0xFFFFFFFF` because they are erased. Older entries need not be invalidated, since the newest one wins. The page is erased once every slot is written. If no valid entry is found, the block is new.

| Old entries | ... | Valid entry | ... | Unused entry |
| ----------- | --- | ----------- | --- | ------------ |
| 0x0         | ... | 0x00001000  | ... | 0xFFFFFFFF   |
#### File Record
A file record describes the location, size and name of a file. The offset is always measured from the start of the flash memory sector. An entry whose offset is `0x0` is invalidated, and an offset of `0xFFFFFFFF` means it has not been written yet.

| Name     | Size       | Note |
| -------- | ---------- | ---- |
| Offset   | uint32     | From the start of the flash memory sector |
| Filesize | uint32     | In bytes; the reserved space is rounded up to whole pages |
| Name     | `Filename` | 8 characters |

The start offset of a file is always page-aligned, and the space reserved for it is rounded up to whole pages; the recorded size is the exact byte count.
#### File Table
The file table holds file records. It is a file itself, managed by this service, and its first record always points to that file, which sets the table's length.

| File record 0     | File record 1 | File record 2     | File record 3 | File record 4 |
| ---------------- | ------------ | ---------------- | ------------ | ------------ |
| Filetable itself | First file   | Invalidated file | Second file  | Unwritten    |

A file is handed to other functions to have data stored in it or read from it, and has its own format.
### Commands (030x)
| Name              | ID | Request                                     | Response                                    | Note                                                  |
| ----------------- | -- | ------------------------------------------- | ------------------------------------------- | ----------------------------------------------------- |
| Format Filesystem | 0  | -                                           | Success flag                              |                                                       |
| Create File       | 1  | Name, Size (>0)                             | Success flag                              | Respond only if requested |
| Delete File       | 2  | Name                                        | Success flag                              | Respond only if requested |
| Resize File       | 3  | Name, New Size (>0)                         | Success flag                              | Respond only if requested |
| Rename File       | 4  | Old Name, New Name                          | Success flag                              | Respond only if requested |
| Read File         | 5  | Name                                        | Fragmentation, Name, File contents (stream) | Maximum 64 byte stream fragment                       |
| Write File        | 6  | Fragmentation, Name, File contents (stream) | Last sequential fragmentation index written | Respond only if requested, maximum 64 byte stream fragment |
### Implementation Functions
#### Main Functions (implement per device, preferably not exposed)
- `bool Init()`
	Finds and opens the storage partition. Returns true if successful.
- `uint32_t Read(uint32_t Address, uint32_t Length, char* Buffer)`
	Reads Length bytes directly from flash at Address, an offset from the start of the flash sector, into Buffer, which must hold at least Length bytes. Returns the number of bytes actually read.
- `bool Write(uint32_t Address, uint32_t Length, char* Buffer)`
	Writes Length bytes directly to flash, from Buffer to Address, an offset from the start of the flash sector. Only 1 to 0 transitions are possible. Returns true if successful.
- `bool Erase(uint32_t Address, uint32_t Length = PAGE_SIZE)`
	Erases the sectors starting at Address, for Length continuous bytes. Length must be page-aligned.
- `bool Format()`
	Wipes the entire storage system.
#### Filesystem Utility Functions (filesystem only, universal)
- `bool FindFiletable(uint32_t* Offset)`
	Reads the file table's location from the first page.
- `bool FindInFiletable(char[8] Filename, uint32_t* Index)`
	Returns the index of the file record in the file table, or `0xFFFFFFFF` if it does not exist.
- `uint32_t FindSpace(uint32_t Size)`
	Finds contiguous space of the requested size, starting from an internal wear cursor, and checking the file table, excluding existing files and the pointer page. Returns the start address of the page found, as an offset from the start of the filesystem.
- `bool GetEndOfFiletable(uint32_t* Index)`
	Returns the first unwritten file record.
- `bool DeleteFileRecord(char[8] Filename)`
	Deletes the file record. Returns true if successful.
- `bool WriteFileRecord(FileRecord NewRecord)`
	Writes a new file record at the end. If no space remains after writing, the file table must be filtered and moved with `MoveFiletable()`.
- `bool MoveFiletable()`
	The table has a fixed size per device and is moved only when it is full. It finds a new location with `FindSpace`, initialises a new file table of the same size at that location, including its self-describing first entry, and copies the valid entries into it. Finally it writes the new pointer to the first page.
#### File Based Functions (accessible outside, universal)
- `bool CreateFile(char[8] Filename, uint32_t Length)`
	Finds available space with `FindSpace`, erases it, and marks it in the file table. Returns true if created.
- `bool DeleteFile(char[8] Filename)`
	Invalidates the file table entry.
- `bool ResizeFile(char[8] Filename, uint32_t NewLength)`
	Changes the file length by extending or shrinking it, checking the file table for whether it can. When possible, only a new file table entry with the increased length is written and the old one invalidated. Shrinking is always possible.
- `bool RenameFile(char[8] OldFilename, char[8] NewFilename)`
	Creates a new record with the new name for the same file, and deletes the old one.
- `uint32_t ReadFromFile(char[8] Filename, uint32_t Offset, uint32_t Length, char* Buffer)`
	Utility wrapper for `Read`. The offset is from the start of the file.
- `bool WriteToFile(char[8] Filename, uint32_t Offset, uint32_t Length, char* Buffer)`
	Safer wrapper for `Write`. The offset is from the start of the file.
- `uint32_t FileExists(char[8] Filename)`
	Returns the file size if the file exists, otherwise `0xFFFFFFFF`. Uses `FindInFiletable`.
