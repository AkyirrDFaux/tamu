// Host-side test for the backup-file write's recovery paths (t_1e301bfb).
//
// The DAS's whole persistent set is one 40 B `.SV`, written by `WriteBackupFile` as
// copy-to-`.SV~`, then rename - two file-table records per save. The table is append-only, so
// after a few saves it is full and the append needs `MoveFiletable()`, which needs
// STORAGE_TABLE_SIZE (128 B = two CONTIGUOUS 64 B blocks). On the DAS the data area is seven
// blocks and a save already holds three of them (the 2-block table, the live `.SV` and its
// staging file), so a store whose free blocks are single-block islands cannot place that move:
// `FindSpace` returns 0, `CreateFile`/`RenameFile` fail, and Save All answers FLAG_FAIL - for
// ever, because the failed save changes nothing. Measured on node 0.4 (card t_1e301bfb): 0x4E
// three times in a row in exactly that store, then 0x2E once the two dummy files were removed.
//
// The real StorageBlockFS.h runs here over a fake 512 B NOR flash, so the store the test builds
// is the store the rig had: 64 B blocks, a 2-page table, create/delete pairs filling the record
// slots. Four stores are checked, in the order the writer prefers them:
//   1. an ordinary store keeps the atomic path - a rewrite leaves one live `.SV` block and the
//      superseded generation invalidated (nothing regresses for the core or a healthy node);
//   2. a NODE-SHAPED store (`.SV` + two 1-block files, table full, one free block once the
//      staging file exists) - the save must RECLAIM: the table is compacted and the store ends up
//      with room again, so the save lands and every other writer works afterwards;
//   3. the deadlock (island) store - no free record slot and a longest free run of one block even
//      after the staging file is released. There the in-place rewrite is the last resort: a
//      >= 20-save run must succeed byte-identically, and the store is asserted to STAY stuck -
//      that residual is the stated cost of the last resort, not something this test pretends
//      away;
//   4. the same deadlock store keeps the RUNNING state after many saves (no block growth).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Types/Enums.h"
#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h"

// The device-layer symbols the storage headers reference; the file layer never needs them.
DeviceStatusStruct DeviceStatus;
void DispatchPacket(const PacketFrame &) {}
void DeviceLog(const char *, const char *, ...) {}
PacketFrame TxFrame;

// The DAS geometry comes from the build flags (STORAGE_FLASH_SIZE=512, STORAGE_BLOCK_SIZE=64,
// MEMORY_BACKUP_CAP=128, BOARD_DAS_v0_1), so the layout exercised here is the layout the node runs.
#include "Core/Functions/MemoryBackup.h"

static uint8_t Flash[STORAGE_FLASH_SIZE];

bool StorageFlashDevice::Init()
{
    memset(Flash, 0xFF, sizeof(Flash));
    return true;
}

uint32_t StorageFlashDevice::Read(uint32_t Offset, void *Data, uint32_t Size)
{
    if (Offset > STORAGE_FLASH_SIZE || Size > STORAGE_FLASH_SIZE - Offset) return 0;
    memcpy(Data, Flash + Offset, Size);
    return Size;
}

// NOR semantics: a program only clears bits (1 -> 0), so a write onto a page that was not erased
// shows up as a wrong read-back instead of being silently accepted.
bool StorageFlashDevice::Write(uint32_t Offset, const void *Data, uint32_t Size)
{
    if (Offset > STORAGE_FLASH_SIZE || Size > STORAGE_FLASH_SIZE - Offset) return false;
    const uint8_t *Src = (const uint8_t *)Data;
    for (uint32_t i = 0; i < Size; i++)
        Flash[Offset + i] &= Src[i];
    return true;
}

bool StorageFlashDevice::Erase(uint32_t Offset, uint32_t Size)
{
    if ((Offset & (STORAGE_BLOCK_SIZE - 1)) || (Size & (STORAGE_BLOCK_SIZE - 1))) return false;
    if (Offset > STORAGE_FLASH_SIZE || Size > STORAGE_FLASH_SIZE - Offset) return false;
    memset(Flash + Offset, 0xFF, Size);
    return true;
}

bool StorageFlashDevice::Format() { return Erase(0, STORAGE_FLASH_SIZE); }

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

// The DAS's persistent mirror name, the same 8-byte record name Core/Services/StaticMemory.h's
// StaticValuesName() returns (that header pulls the whole block model, which this test does not
// need; the name is the storage layer's input either way).
static const char SvNameBytes[8] = {'.', 'S', 'V', ' ', ' ', ' ', ' ', ' '};

static const char *SvName() { return SvNameBytes; }

static bool SvBytes(uint8_t Out[40]) { return ReadBackupFile(SvName(), Out, 40) == 40; }

// One bit per flash block: the pointer page, the file table and every live file (walked from the
// store itself, so the map never depends on the test knowing the file names).
static uint8_t Taken[STORAGE_FLASH_SIZE / STORAGE_BLOCK_SIZE];

static void BuildTaken()
{
    memset(Taken, 0, sizeof(Taken));
    Taken[0] = 1; // the first-page table pointer is never allocatable
    for (uint32_t B = Storage.FileTableOffset / STORAGE_BLOCK_SIZE;
         B < (Storage.FileTableOffset + Storage.FileTableSize) / STORAGE_BLOCK_SIZE; B++)
        Taken[B] = 1;
    Storage.ForEachFile([](const char *Name) {
        uint32_t Off = 0, Size = 0;
        if (!Storage.GetFileInfo(Name, &Off, &Size)) return;
        uint32_t First = Off / STORAGE_BLOCK_SIZE;
        uint32_t Last = (Off + BlocksForSize(Size) * STORAGE_BLOCK_SIZE) / STORAGE_BLOCK_SIZE;
        for (uint32_t B = First; B < Last && B < sizeof(Taken); B++) Taken[B] = 1;
    });
}

// Number of free data blocks (block 0 is the pointer page, never allocatable).
static uint32_t FreeBlocks()
{
    BuildTaken();
    uint32_t Free = 0;
    for (uint32_t B = 1; B < STORAGE_FLASH_SIZE / STORAGE_BLOCK_SIZE; B++)
        if (!Taken[B]) Free++;
    return Free;
}

// The longest contiguous free run, in blocks - a table move needs two.
static uint32_t LongestFreeRun()
{
    BuildTaken();
    uint32_t Best = 0, Run = 0;
    for (uint32_t B = 1; B < STORAGE_FLASH_SIZE / STORAGE_BLOCK_SIZE; B++)
    {
        if (Taken[B]) { if (Run > Best) Best = Run; Run = 0; continue; }
        Run++;
    }
    return Run > Best ? Run : Best;
}

static bool TableFull()
{
    uint32_t Slot = 0;
    return Storage.GetEndOfFiletable(&Slot) && Slot >= Storage.TableCapacity();
}

// Fills the record slots with create/delete pairs, so the table ends up with no free slot and
// only invalidated holes - the state every save after the fourth one runs into.
static void FillRecordSlots()
{
    for (char C = '0'; C <= '9'; C++)
    {
        char Name[3] = {'X', C, 0};
        uint32_t Slot = 0;
        if (!Storage.GetEndOfFiletable(&Slot) || Slot >= Storage.TableCapacity()) break;
        if (!Storage.CreateFile(Name, 64)) break;
        Storage.DeleteFile(Name);
    }
}

int main()
{
    uint8_t First[40], Second[40], Got[40];
    for (int i = 0; i < 40; i++) { First[i] = (uint8_t)(0x10 + i); Second[i] = (uint8_t)(0x80 + i); }

    // ---- 1. An ordinary store: the atomic copy-and-rename path keeps its shape. ----
    Storage.Init();
    CHECK(Storage.FileTableOffset != 0);

    CHECK(WriteBackupFile(SvName(), First, 40));
    CHECK(SvBytes(Got));
    CHECK(memcmp(Got, First, 40) == 0);
    CHECK(WriteBackupFile(SvName(), Second, 40));
    CHECK(SvBytes(Got));
    CHECK(memcmp(Got, Second, 40) == 0);
    // One live generation: the superseded `.SV` record is invalidated, so only its successor's
    // block is still in use (40 B = one 64 B block).
    CHECK(Storage.UsedFlashBytes() == STORAGE_BLOCK_SIZE);

    // ---- 2. A node-shaped store: full table, one free block once the staging file exists. ----
    // `.SV` plus two 1-block files, so the free space is narrower than the move needs at the
    // moment the append happens - but wide enough again once the staging file is released. This
    // is the store the reclaim step exists for: it must come out HEALTHY, not merely saved.
    Storage.Format();
    CHECK(WriteBackupFile(SvName(), First, 40));
    CHECK(Storage.CreateFile("F1", 64));
    CHECK(Storage.CreateFile("F2", 64));
    FillRecordSlots();
    CHECK(TableFull());
    CHECK(FreeBlocks() == 2);            // the staging file takes one, leaving one for a move

    CHECK(WriteBackupFile(SvName(), Second, 40));
    CHECK(SvBytes(Got));
    CHECK(memcmp(Got, Second, 40) == 0);
    // Reclaimed, not just rewritten: the table was compacted, so it has room again and the store
    // left the state in which every later append would have needed an unplaceable move.
    CHECK(!TableFull());
    CHECK(Storage.CreateFile("T4", 64)); // a plain CreateFile works again (the reviewer's residual)

    // A 20-save run in that same store: every save must land and the file must always read back
    // what was written last - the reclaim never loses a generation, and when the free space is
    // narrow again the last resort keeps the run going.
    for (int i = 0; i < 20; i++)
    {
        const uint8_t *Want = (i & 1) ? First : Second;
        CHECK(WriteBackupFile(SvName(), Want, 40));
        CHECK(SvBytes(Got));
        CHECK(memcmp(Got, Want, 40) == 0);
    }
    // No space leak over the whole run: the 40 B file holds one block and so does each dummy
    // file, so nothing the reclaim or the last resort did left a block unaccounted for.
    CHECK(Storage.UsedFlashBytes() == 4 * STORAGE_BLOCK_SIZE);

    // ---- 3. The deadlock (island) store: no free slot AND no two contiguous free blocks. ----
    Storage.Format();
    CHECK(WriteBackupFile(SvName(), First, 40)); // the live `.SV` the fallback rewrites in place
    CHECK(Storage.CreateFile("F1", 64));
    CHECK(Storage.CreateFile("T1", 64));
    CHECK(Storage.DeleteFile("T1"));             // a hole between the two live files
    CHECK(Storage.CreateFile("F2", 64));
    FillRecordSlots();

    CHECK(TableFull());                      // no free record slot: the next record needs a move
    CHECK(LongestFreeRun() < 2);             // and the move cannot be placed, even after reclaim
    uint32_t UsedBefore = Storage.UsedFlashBytes();

    // 20 consecutive saves - the long run, not two: every one must land and read back exactly.
    for (int i = 0; i < 20; i++)
    {
        const uint8_t *Want = (i & 1) ? First : Second;
        CHECK(WriteBackupFile(SvName(), Want, 40));
        CHECK(SvBytes(Got));
        CHECK(memcmp(Got, Want, 40) == 0);
    }
    // The stated residual of the last resort: the save keeps working, but the store itself is
    // unchanged - table still full, free blocks still islands, so another writer still cannot
    // create a file here. Nothing hides that; recovery needs a block to be freed.
    CHECK(TableFull());
    CHECK(LongestFreeRun() < 2);
    CHECK(!Storage.CreateFile("T4", 64));
    // ...and no space leak: the 40 B file still occupies one block, the two dummy files theirs.
    CHECK(Storage.UsedFlashBytes() == UsedBefore);
    CHECK(UsedBefore == 3 * STORAGE_BLOCK_SIZE);

    std::printf("%s: %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
