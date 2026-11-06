#pragma once

// Backup-file helpers (the backup-name/read/write trio)
//
// Part of Memory.h, split for readability; included from there.

#include <cstdint>
#include <cstring>
#include "Core/Functions/MemoryTypes.h"
#include "Core/Functions/Packet.h"
#include "Core/Functions/Storage.h"

void DispatchPacket(const PacketFrame &Frame);

#define INVALID_BLOCK 0xFF
#define INVALID_INDEX 0xFF

// Capacity of a serialised service backup buffer (the `.SV` static space and the `.DT_`/`.DV_`
// dynamic files). RAM-starved devices (DAS) build with a smaller value via the MEMORY_BACKUP_CAP
// build flag.
#ifndef MEMORY_BACKUP_CAP
#define MEMORY_BACKUP_CAP 2048
#endif

// A block number is the dynamic memory's single global index (0..255). The service a request
// targets is carried by the packet's SRV TGT (ServiceType), never by the block number itself.
struct BlockIndex
{
    uint8_t Block = INVALID_BLOCK;
    uint8_t Field = INVALID_INDEX;
    uint8_t Key = INVALID_INDEX;
    uint8_t Padding = 0;
};

// Sends a single response packet back to the requester (only if REQACK was set).
// `status` carries FLAG_SUCCESS or FLAG_FAIL for a status reply: the flag replaces the payload, and
// a payload may follow it to carry detail (Docs/RSBus and Packets.md).
__attribute__((noinline)) void SendResponse(const PacketFrame &Frame, const uint8_t *Payload, uint16_t Len,
                                            uint8_t Status = 0)
{
    if (!(Frame.Flags & FLAG_REQACK))
        return;
    PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                     (uint8_t)(FLAG_TYPE | FLAG_START | FLAG_STOP | Status), Payload, Len);
    DispatchPacket(TxFrame);
}

// Emits one FRAG fragment: the caller has placed `n` content bytes at tx_frame.payload + 4,
// and the 4-byte frag info is written before the frame is finalized and dispatched. Every
// streamed reply (enumerate, storage read, subscription tables, SNDB read-all, logs) uses it.
__attribute__((noinline)) void SendFragFragment(const PacketFrame &Frame, uint16_t Frag,
                                                uint16_t Frags, uint16_t Len,
                                                uint8_t Priority = PRIORITY_STREAM)
{
    uint8_t Flags = FLAG_TYPE | FLAG_FRAG;
    if (Frag == 0) Flags |= FLAG_START;
    if (Frag == Frags - 1) Flags |= FLAG_STOP;
    WriteFragInfo(TxFrame.Payload, Frag, Frags);
    FinalizeReply(TxFrame, Frame, Flags, (uint16_t)(4 + Len), Priority);
    DispatchPacket(TxFrame);
}

// The same fragment, addressed by the request's routing fields instead of the request frame: a
// reply that outlives the request (the resumable storage read keeps a cursor across app-task
// passes) can only carry target/CMD/TRID. Produces exactly the frame SendFragFragment would.
__attribute__((noinline)) void SendFragTo(uint16_t Tgt, uint16_t Cmd, uint16_t Trid,
                                          uint16_t Frag, uint16_t Frags, uint16_t Len,
                                          uint8_t Priority = PRIORITY_STREAM)
{
    uint8_t Flags = FLAG_TYPE | FLAG_FRAG;
    if (Frag == 0) Flags |= FLAG_START;
    if (Frag == Frags - 1) Flags |= FLAG_STOP;
    WriteFragInfo(TxFrame.Payload, Frag, Frags);
    FinalizeReplyTo(TxFrame, Tgt, Cmd, Trid, Flags, (uint16_t)(4 + Len), Priority);
    DispatchPacket(TxFrame);
}

// A status reply (success/fail, no payload) to a stored request identity - the read stream's
// failure answer once the request frame is gone.
__attribute__((noinline)) void SendStatusTo(uint16_t Tgt, uint16_t Cmd, uint16_t Trid, uint8_t Status)
{
    FinalizeReplyTo(TxFrame, Tgt, Cmd, Trid,
                    (uint8_t)(FLAG_TYPE | FLAG_START | FLAG_STOP | Status), 0);
    DispatchPacket(TxFrame);
}

// Replies with the success or fail packet flag and no payload. The flag is the whole reply;
// a caller with more to say appends a payload after it.
// Failures are logged on the core (DeviceLog is a no-op on textless nodes): the
// service tag, CID and the request's block/field/key give a full audit trail for
// every rejected memory operation without per-call-site logging.
__attribute__((noinline)) void RespondStatus(const PacketFrame &Frame, bool Ok)
{
    if (!Ok)
    {
#ifndef DEVICE_LOG_TEXTLESS
        const char *Tag = "MEM";
        switch (GetServiceType(Frame.Cmd))
        {
            case ServiceType::Storage:        Tag = "STORAGE"; break;
            default: break;
        }
        uint16_t Block = INVALID_INDEX, Field = INVALID_INDEX, Key = INVALID_INDEX;
        if (PayloadBytes(Frame) >= sizeof(BlockIndex))
        {
            const BlockIndex *Idx = reinterpret_cast<const BlockIndex *>(Frame.Payload);
            Block = Idx->Block; Field = Idx->Field; Key = Idx->Key;
        }
        DeviceLog(Tag, "CID %u failed block=%u field=%u key=%u",
                  (unsigned)GetServiceCID(Frame.Cmd),
                  (unsigned)Block, (unsigned)Field, (unsigned)Key);
#endif
        // Structured report (LogHandler CID 0): reaches the core's log DB even
        // from textless nodes; code = CID so failures dedup per service+op.
        ReportLog(MakeLog(false, (uint8_t)GetServiceType(Frame.Cmd), GetServiceCID(Frame.Cmd), 0), PRIORITY_ERROR);
    }
    SendResponse(Frame, nullptr, 0, (uint8_t)(Ok ? FLAG_SUCCESS : FLAG_FAIL));
}

// Derives the staging-file name for an atomic backup update: the last character of the
// padded 8-byte name becomes '~' (e.g. ".SV     " -> ".SV    ~"). Backup names never end in '~'.
inline void BackupTempName(const char Name[8], char Out[8])
{
    memcpy(Out, Name, 8);
    Out[7] = '~';
}

// Writes `data` to the backup file `name` atomically (NOR-safe copy-and-rename):
//
// The staging file costs TWO file-table records (its own, then the rename's) and, once the table
// is full, a table MOVE - and a move needs STORAGE_TABLE_SIZE of CONTIGUOUS space while the live
// file and the staging file each hold a block of their own. On the DAS (512 B store, 64 B blocks,
// five usable blocks) that peak is exactly the whole data area, so a store whose free blocks are
// single-block islands can never place it: CreateFile/RenameFile fail, and because the failure
// changes nothing, every later save fails the same way. Measured on node 0.4 (t_1e301bfb): Save
// All answered FLAG_FAIL three times in a row in that store and 0x2E as soon as the two stray
// files that fragmented it were removed.
//
// Two steps keep a store out of that state (in this order):
//
//  1. RECLAIM. The append that failed needed a table move, so the table is full of the records
//     earlier saves left behind (every save appends two and never reuses a slot). The failed
//     attempt has already released its staging block, which is exactly the space a move was
//     missing: compacting the table (MoveFiletable copies the valid records densely, so the
//     superseded generations disappear, and the old table's pages are freed) puts the store back
//     in a healthy state and the save is retried once. This is what stops a node's store from
//     staying stuck for every other writer, not just for this one.
//  2. LAST RESORT. When even that cannot be placed (the data area itself has no two contiguous
//     free blocks, e.g. a store holding several 1-block files), the live file is rewritten in
//     place: one erase, one write, no record and no move. That gives up atomicity for the ~ms of
//     the write (a power cut there leaves a torn file), which is the right trade in that state -
//     the alternative is that the save never succeeds and the values stay in RAM until the next
//     power cycle. It is only taken for a file that already exists at exactly this length, so it
//     never has to grow a file or invent a record.
//
// The residual of step 2 is stated rather than hidden: a store that reaches it keeps saving
// (every save succeeds, byte-identical reads) but its table stays full and its free blocks stay
// islands, so a plain CreateFile keeps failing there until something frees blocks. Step 1 is what
// a node's own stores hit, because they hold one `.SV` (plus at most a file or two).

// Stages `len` bytes under the staging name `tmp` and commits them under `name`: one create, one
// write, one record append (the rename). Removes the staging file when any step fails.
inline bool StageBackupFile(const char Name[8], const char Tmp[8], const uint8_t *Data, uint16_t Len)
{
    // Remove a staging file left over from an interrupted update: CreateFile refuses a duplicate
    // name, so a leftover would block this save too.
    if (Storage.FileExists(Tmp) != 0xFFFFFFFF)
        Storage.DeleteFile(Tmp);

    if (Storage.CreateFile(Tmp, Len) &&
        Storage.WriteToFile(Tmp, 0, Len, (const char *)Data) &&
        Storage.RenameFile(Tmp, Name))
        return true;

    Storage.DeleteFile(Tmp); // don't leave an orphan staging file behind
    return false;
}

inline bool WriteBackupFile(const char Name[8], const uint8_t *Data, uint16_t Len)
{
    char Tmp[8];
    BackupTempName(Name, Tmp);
    if (memcmp(Name, Tmp, 8) == 0)
        return false; // naming convention violation guard

    // Stage the new generation in its own file (CreateFile provides freshly erased blocks) and
    // commit it atomically under its final name.
    if (StageBackupFile(Name, Tmp, Data, Len))
        return true;

    // Reclaim: only a FULL table needs a move, so only then can compacting it be the thing that
    // makes the write placeable. The staging block is already gone (StageBackupFile released it),
    // so a move that had no room before now has the block it was short of.
    uint32_t End = 0;
    if (Storage.GetEndOfFiletable(&End) && End >= Storage.TableCapacity() &&
        Storage.CompactFiletable() && StageBackupFile(Name, Tmp, Data, Len))
        return true;

    // Last resort: rewrite the live file in place, keeping the save working in a store whose data
    // area has no two contiguous free blocks left (see the residual above).
    uint32_t Offset, Size;
    if (!Storage.GetFileInfo(Name, &Offset, &Size) || Size != Len)
        return false;
    if (!StorageFlash.Erase(Offset, BlocksForSize(Len) * STORAGE_BLOCK_SIZE))
        return false;
    return Storage.WriteToFile(Name, 0, Len, (const char *)Data);
}

// Reads a backup file into `out`; returns the byte count (0 if absent or too large).
inline uint16_t ReadBackupFile(const char Name[8], uint8_t *Out, uint16_t Cap)
{
    return (uint16_t)Storage.ReadFromFile(Name, 0, Cap, (char *)Out);
}

