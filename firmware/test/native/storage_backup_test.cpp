// Host-side test for the backup-file write's last-resort path (t_1e301bfb).
//
// The DAS's whole persistent set is one 40 B `.SV`, written by `WriteBackupFile` as
// copy-to-`.SV~`, then rename - two file-table records per save. The table is append-only, so
// after a few saves it is full and `MoveFiletable()` has to run, which needs
// STORAGE_TABLE_SIZE (128 B = two CONTIGUOUS 64 B blocks). On the DAS the data area is five
// blocks and a save already holds two of them (the live `.SV` and its staging file), so a store
// whose free blocks are single-block islands cannot place that move: `FindSpace` returns 0,
// `CreateFile`/`RenameFile` fail, and Save All answers FLAG_FAIL - for ever, because the failed
// save changes nothing. Measured on node 0.4 (card t_1e301bfb): 0x4E three times in a row in
// exactly that store, then 0x2E once the two dummy files were removed.
//
// The real StorageBlockFS.h runs here over a fake 512 B NOR flash, so the store the test builds
// is the store the rig had: 64 B blocks, a 2-page table, create/delete pairs filling the record
// slots and two live 1-block files fragmenting the free space. The assertions:
//   1. an ordinary store keeps the atomic path - a rewrite leaves one live `.SV` block and the
//      superseded generation invalidated (nothing regresses for the core or a healthy node);
//   2. the deadlock store is really built (no free record slot, longest free run < 2 blocks) -
//      otherwise a pass would only mean the state was never reached;
//   3. `WriteBackupFile` still returns true there and the stored bytes are the new ones (the
//      last-resort in-place write), and it keeps working on the next save.

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

static void MarkFile(uint8_t Taken[], const char *Name)
{
    uint32_t Off, Size;
    if (!Storage.GetFileInfo(Name, &Off, &Size)) return;
    uint32_t Last = Off + BlocksForSize(Size) * STORAGE_BLOCK_SIZE;
    for (uint32_t B = Off / STORAGE_BLOCK_SIZE; B < Last / STORAGE_BLOCK_SIZE; B++)
        Taken[B] = 1;
}

// The pointer page (block 0) is never allocatable. Everything else in the flash region is a data
// block; the table and the live files are marked, and the free blocks are returned as run lengths
// (a table move needs a run of two).
static uint32_t LongestFreeRun()
{
    uint8_t Taken[STORAGE_FLASH_SIZE / STORAGE_BLOCK_SIZE];
    memset(Taken, 0, sizeof(Taken));
    for (uint32_t B = Storage.FileTableOffset / STORAGE_BLOCK_SIZE;
         B < (Storage.FileTableOffset + Storage.FileTableSize) / STORAGE_BLOCK_SIZE; B++)
        Taken[B] = 1;
    MarkFile(Taken, "F1");
    MarkFile(Taken, "F2");
    MarkFile(Taken, SvName());

    uint32_t Best = 0, Run = 0;
    for (uint32_t B = 1; B < STORAGE_FLASH_SIZE / STORAGE_BLOCK_SIZE; B++)
    {
        if (Taken[B]) { if (Run > Best) Best = Run; Run = 0; continue; }
        Run++;
    }
    return Run > Best ? Run : Best;
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

    // ---- 2. The deadlock store: full table, no two contiguous free blocks. ----
    Storage.Format();
    CHECK(WriteBackupFile(SvName(), First, 40)); // the live `.SV` the fallback rewrites in place
    CHECK(Storage.CreateFile("F1", 64));
    CHECK(Storage.CreateFile("T1", 64));
    CHECK(Storage.DeleteFile("T1"));             // a hole between the two live files
    CHECK(Storage.CreateFile("F2", 64));
    for (char C = '0'; C <= '9'; C++)            // fill the record slots with create/delete holes
    {
        char Name[3] = {'X', C, 0};
        uint32_t Slot = 0;
        if (!Storage.GetEndOfFiletable(&Slot) || Slot >= Storage.TableCapacity()) break;
        if (!Storage.CreateFile(Name, 64)) break;
        Storage.DeleteFile(Name);
    }

    uint32_t Slot = 0;
    CHECK(Storage.GetEndOfFiletable(&Slot));
    CHECK(Slot >= Storage.TableCapacity()); // no free record slot: the next record needs a move
    CHECK(LongestFreeRun() < 2);            // and no two contiguous free blocks for that move

    // ---- 3. The write the copy-and-rename cannot place still lands, in place, and repeats. ----
    CHECK(WriteBackupFile(SvName(), Second, 40));
    CHECK(SvBytes(Got));
    CHECK(memcmp(Got, Second, 40) == 0);
    CHECK(WriteBackupFile(SvName(), First, 40));
    CHECK(SvBytes(Got));
    CHECK(memcmp(Got, First, 40) == 0);

    std::printf("%s: %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
