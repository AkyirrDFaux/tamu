One large fixed chunks, with vastly different possible size (4kB-2MB).
NOR flash (1 erased, 0 filled), can be slow.
Should be implemented per-device, common interface required.
### File system
Aligned with pages (smallest erasable unit).

First page (low wear) - Pointer to start of file table only, first valid entry is it, old entries are 0x0, unused entries are 0xFFFFFFFF (erased), block is only erased when last entry is invalidated.
If no valid entry is found, the block is new.

| Old entries |     | Valid entry | ... | Unused entry |
| ----------- | --- | ----------- | --- | ------------ |
| 0x0         | ... | 0x53451345  | ... | 0xFFFFFFFF   |
#### File record
Contains information about the location, size and name of file.
Offset is always from start of flash memory sector, Entry with offset 0x0 is invalidated, with 0xFFFFFFFF is unwritten yet.

| Offset | Filesize | Name           |
| ------ | -------- | -------------- |
| 32bit  | 32bit    | 8 bytes (Text) |
Start offset of file is always aligned with a page, it's size is a multiple of a page.
#### File table
File table contains file records. It's a file itself managed by this service, first records always points to this file (therefore sets length).

| Filerecord 0     | Filerecord 1 | Filerecord 2     | Filerecord 3 | Filerecord 4 |
| ---------------- | ------------ | ---------------- | ------------ | ------------ |
| Filetable itself | First file   | Invalidated file | Second file  | Unwritten    |
File is given off to have data stored or read by other functions, it has it's own format.
### Commands (030x)

| Function          | CID | Payload In                                  | Payload out                                  | Note                      |
| ----------------- | --- | ------------------------------------------- | -------------------------------------------- | ------------------------- |
| Format Filesystem | 0   | -                                           | Success                                      |                           |
| Create File       | 1   | Name, Size (>0)                             | Success (bool)                               | respond only if requested |
| Delete File       | 2   | Name                                        | Success (bool)                               | respond only if requested |
| Resize File       | 3   | Name, New Size (>0)                         | Success (bool)                               | respond only if requested |
| Rename File       | 4   | Old Name, New Name                          | Success (bool)                               | respond only if requested |
| Read File         | 5   | Name                                        | Name, Fragmentation, File contents (stream)  |                           |
| Write File        | 6   | Name, Fragmentation, File contents (stream) | Last sequential fragmentation index written. | respond only if requested |


### Functions to implement
#### Main functions (implement per device, prefferably do not expose)
- `uint32_t Read(uint32_t Address, uint32_t Length, char* Buffer)`
	Direct read from flash at Address (offset from flash sector start) to Buffer (must be at least Length bytes)), Length of bytes, return number of bytes actually read
- `bool Write(uint32_t Address, uint32_t Length, char* Buffer)`
	Direct write to flash (1->0) from Buffer, Length of bytes, to Address (offset from flash sector start), return true if successful
- `bool Erase (uint32_t Address, uint32_t Length = PAGE_SIZE)`
	Direct erasure of sector starting at Address, Length of continuous bytes (has to be page aligned).
-  `bool Format()`
	Wipes the entrire storage system.
#### Filesystem utility functions (filesystem only, universal)
- `uint32_t FindFiletable()`
	Reads filetable location from the first page.
- `uint32_t FindInFiletable(char[8] Filename)`
	Returns the index of the Filerecord in the filetable if exists, 0xFFFFFFFF is none.
- `uint32_t FindSpace(uint32_t Size)`
	Finds contiguous space of specified size starting at a start page by checking the filetable (existing files and pointer page excluded). Even out flash wear. Returns the start address (offset from filesystem start) of the page found.
- `uint32_t GetEndOfFiletable()`
	Returns the first non-written filerecord.
- `bool DeleteFilerecord(char[8] Filename)`
	Deletes the file record, true if successful.
- `bool WriteFilerecord(Filerecord NewRecord)`
	Writes a new Filerecord to the end. If no more space is avaliable after writing, the filetable needs to be filtered and moved using `MoveFiletable()`.
- `bool MoveFiletable()`
	Counts valid entries, if >75% full (valid entries take more than x% of total filetable size) increase size by one page, if < 25% full decrease size by one page (1 page is minimum), find a new space using `FindSpace`, initialize a new filetable (with it's selfdescribing first entry) at the new location, and copy valid entries to the new table.
	Lastly write the new pointer to the first page, and erase the old pointer from there.
#### File based functions (Accessible outside, universal)
- `bool CreateFile(char[8] Filename, uint32 Length)`
	Finds avaliable space using `FindSpace`, erases it, marks it in the file table. Returns true if created.
- `bool DeleteFile(char[8] Filename)
	Invalidates the file table entry.
- `bool ResizeFile(char[8] Filename, uint32 NewLength)`
	Tries to change the file length by just extending/shrinking it, checks the filetable if it can. If possible, only write a new (with increased length) filetable entry and invalidate old one. Shrinking is always possible.
-  `bool RenameFile(char [8] OldFilename, char [8] NewFilename)`
	Creates a new record with new name for the same file, deletes the old one.
- `uint32_t ReadFromFile(char[8] Filename, uint32_t Offset, uint32_t Length, char* Buffer)`
	Utility wrapper for the `Read`.
	Offset is from file start.
- `bool WriteToFile(char[8] Filename, uint32_t Offset, uint32_t Length, char* Buffer)`
	Safer wrapper for `Write`.
	Offset is from file start.
- `uint32_t FileExists(char[8] Filename)`
	If it exists returns filesize, otherwise 0xFFFFFFFF. Uses `FindInFiletable`.