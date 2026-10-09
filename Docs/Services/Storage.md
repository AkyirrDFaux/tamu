Storage holds large chunks whose size varies widely, from 4 kB to 2 MB. It sits on NOR flash, where an erased bit reads 1 and a written bit reads 0, and access can be slow. It is implemented per device, behind a common interface.
### File System
Aligned with pages, the smallest erasable unit.

The first page, kept for low wear, holds only a pointer to the start of the file table. Its first valid entry is that pointer, old entries are `0x0`, and unused entries read `0xFFFFFFFF` because they are erased. The block is erased only when the last entry is invalidated. If no valid entry is found, the block is new.

| Old entries | ... | Valid entry | ... | Unused entry |
| ----------- | --- | ----------- | --- | ------------ |
| 0x0         | ... | 0x53451345  | ... | 0xFFFFFFFF   |
#### File Record
A file record describes the location, size and name of a file. The offset is always measured from the start of the flash memory sector. An entry whose offset is `0x0` is invalidated, and an offset of `0xFFFFFFFF` means it has not been written yet.

| Name     | Size       | Note |
| -------- | ---------- | ---- |
| Offset   | uint32     | From the start of the flash memory sector |
| Filesize | uint32     | A multiple of a page |
| Name     | `Filename` | 8 characters |

The start offset of a file is always page-aligned, and its size is a multiple of a page.
#### File Table
The file table holds file records. It is a file itself, managed by this service, and its first record always points to that file, which sets the table's length.

| Filerecord 0     | Filerecord 1 | Filerecord 2     | Filerecord 3 | Filerecord 4 |
| ---------------- | ------------ | ---------------- | ------------ | ------------ |
| Filetable itself | First file   | Invalidated file | Second file  | Unwritten    |

A file is handed to other functions to have data stored in it or read from it, and has its own format.
### Commands (030x)
| Name              | ID | Request                                     | Response                                    | Note                                                  |
| ----------------- | -- | ------------------------------------------- | ------------------------------------------- | ----------------------------------------------------- |
| Format Filesystem | 0  | -                                           | Success (bool)                              |                                                       |
| Create File       | 1  | Name, Size (>0)                             | Success (bool)                              | Respond only if requested, not in reduced file system |
| Delete File       | 2  | Name                                        | Success (bool)                              | Respond only if requested, not in reduced file system |
| Resize File       | 3  | Name, New Size (>0)                         | Success (bool)                              | Respond only if requested, not in reduced file system |
| Rename File       | 4  | Old Name, New Name                          | Success (bool)                              | Respond only if requested, not in reduced file system |
| Read File         | 5  | Name                                        | Name, Fragmentation, File contents (stream) | Maximum 64 byte stream fragment                       |
| Write File        | 6  | Name, Fragmentation, File contents (stream) | Last sequential fragmentation index written | Respond only if requested, maximum 64 byte stream fragment |
### Implementation Functions
#### Main Functions (implement per device, preferably not exposed)
- `uint32_t Read(uint32_t Address, uint32_t Length, char* Buffer)`
	Reads Length bytes directly from flash at Address, an offset from the start of the flash sector, into Buffer, which must hold at least Length bytes. Returns the number of bytes actually read.
- `bool Write(uint32_t Address, uint32_t Length, char* Buffer)`
	Writes Length bytes directly to flash, from Buffer to Address, an offset from the start of the flash sector. Only 1 to 0 transitions are possible. Returns true if successful.
- `bool Erase(uint32_t Address, uint32_t Length = PAGE_SIZE)`
	Erases the sectors starting at Address, for Length continuous bytes. Length must be page-aligned.
- `bool Format()`
	Wipes the entire storage system.
#### Filesystem Utility Functions (filesystem only, universal)
- `uint32_t FindFiletable()`
	Reads the file table's location from the first page.
- `uint32_t FindInFiletable(char[8] Filename)`
	Returns the index of the file record in the file table, or `0xFFFFFFFF` if it does not exist.
- `uint32_t FindSpace(uint32_t Size)`
	Finds contiguous space of the requested size, starting from a start page and checking the file table, excluding existing files and the pointer page, and evening out flash wear. Returns the start address of the page found, as an offset from the start of the filesystem.
- `uint32_t GetEndOfFiletable()`
	Returns the first unwritten file record.
- `bool DeleteFilerecord(char[8] Filename)`
	Deletes the file record. Returns true if successful.
- `bool WriteFilerecord(Filerecord NewRecord)`
	Writes a new file record at the end. If no space remains after writing, the file table must be filtered and moved with `MoveFiletable()`.
- `bool MoveFiletable()`
	Counts the valid entries. If the table is more than 75% full it grows by one page, and if it is less than 25% full it shrinks by one page, to a minimum of one page. Finds a new location with `FindSpace`, initialises a new file table at that location, including its self-describing first entry, and copies the valid entries into it. Finally writes the new pointer to the first page and erases the old pointer from it.
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
