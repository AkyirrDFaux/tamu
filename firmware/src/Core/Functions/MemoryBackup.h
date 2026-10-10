#pragma once

// Backup-file helpers (the backup-name/read/write trio)
//
// Part of Memory.h, split for readability; included from there.

#include <cstdint>
#include <cstddef>
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
inline bool WriteBackupFile(const char Name[8], const uint8_t *Data, uint16_t Len)
{
    char Tmp[8];
    BackupTempName(Name, Tmp);
    if (memcmp(Name, Tmp, 8) == 0)
        return false; // naming convention violation guard

    // Remove a staging file left over from an interrupted update.
    if (Storage.FileExists(Tmp) != 0xFFFFFFFF)
        Storage.DeleteFile(Tmp);

    // Stage the new generation in its own file (CreateFile provides freshly erased blocks).
    if (!Storage.CreateFile(Tmp, Len))
        return false;
    if (!Storage.WriteToFile(Tmp, 0, Len, (const char *)Data))
    {
        Storage.DeleteFile(Tmp); // don't leave an orphan staging file behind
        return false;
    }

    // Commit atomically: the staging file becomes the live backup under its final name.
    if (!Storage.RenameFile(Tmp, Name))
    {
        Storage.DeleteFile(Tmp);
        return false;
    }
    return true;
}

// Reads a backup file into `out`; returns the byte count (0 if absent or too large).
inline uint16_t ReadBackupFile(const char Name[8], uint8_t *Out, uint16_t Cap)
{
    return (uint16_t)Storage.ReadFromFile(Name, 0, Cap, (char *)Out);
}

