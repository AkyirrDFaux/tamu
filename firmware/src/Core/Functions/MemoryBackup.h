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

void DispatchPacket(const PacketFrame &frame);

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
__attribute__((noinline)) void SendResponse(const PacketFrame &frame, const uint8_t *payload, uint16_t len,
                                            uint8_t status = 0)
{
    if (!(frame.flags & FLAG_REQACK))
        return;
    PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                     (uint8_t)(FLAG_TYPE | FLAG_START | FLAG_STOP | status), payload, len);
    DispatchPacket(tx_frame);
}

// Emits one FRAG fragment: the caller has placed `n` content bytes at tx_frame.payload + 4,
// and the 4-byte frag info is written before the frame is finalized and dispatched. Every
// streamed reply (enumerate, storage read, subscription tables, SNDB read-all, logs) uses it.
__attribute__((noinline)) void SendFragFragment(const PacketFrame &frame, uint16_t f,
                                                uint16_t frags, uint16_t n,
                                                uint8_t priority = PRIORITY_STREAM)
{
    uint8_t flags = FLAG_TYPE | FLAG_FRAG;
    if (f == 0) flags |= FLAG_START;
    if (f == frags - 1) flags |= FLAG_STOP;
    WriteFragInfo(tx_frame.payload, f, frags);
    FinalizeReply(tx_frame, frame, flags, (uint16_t)(4 + n), priority);
    DispatchPacket(tx_frame);
}

// Replies with the success or fail packet flag and no payload. The flag is the whole reply;
// a caller with more to say appends a payload after it.
// Failures are logged on the core (DeviceLog is a no-op on textless nodes): the
// service tag, CID and the request's block/field/key give a full audit trail for
// every rejected memory operation without per-call-site logging.
__attribute__((noinline)) void RespondStatus(const PacketFrame &frame, bool ok)
{
    if (!ok)
    {
#ifndef DEVICE_LOG_TEXTLESS
        const char *tag = "MEM";
        switch (GetServiceType(frame.srv_tgt))
        {
            case ServiceType::Storage:        tag = "STORAGE"; break;
            default: break;
        }
        uint16_t block = INVALID_INDEX, field = INVALID_INDEX, key = INVALID_INDEX;
        if (PayloadBytes(frame) >= sizeof(BlockIndex))
        {
            const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);
            block = idx->Block; field = idx->Field; key = idx->Key;
        }
        DeviceLog(tag, "CID %u failed block=%u field=%u key=%u",
                  (unsigned)GetServiceCID(frame.srv_tgt),
                  (unsigned)block, (unsigned)field, (unsigned)key);
#endif
        // Structured report (LogHandler CID 0): reaches the core's log DB even
        // from textless nodes; code = CID so failures dedup per service+op.
        ReportLog(MakeLog(false, (uint8_t)GetServiceType(frame.srv_tgt), GetServiceCID(frame.srv_tgt), 0));
    }
    SendResponse(frame, nullptr, 0, (uint8_t)(ok ? FLAG_SUCCESS : FLAG_FAIL));
}

// Derives the staging-file name for an atomic backup update: the last character of the
// padded 8-byte name becomes '~' (e.g. ".SV     " -> ".SV    ~"). Backup names never end in '~'.
inline void BackupTempName(const char name[8], char out[8])
{
    memcpy(out, name, 8);
    out[7] = '~';
}

// Writes `data` to the backup file `name` atomically (NOR-safe copy-and-rename):
inline bool WriteBackupFile(const char name[8], const uint8_t *data, uint16_t len)
{
    char tmp[8];
    BackupTempName(name, tmp);
    if (memcmp(name, tmp, 8) == 0)
        return false; // naming convention violation guard

    // Remove a staging file left over from an interrupted update.
    if (Storage.FileExists(tmp) != 0xFFFFFFFF)
        Storage.DeleteFile(tmp);

    // Stage the new generation in its own file (CreateFile provides freshly erased blocks).
    if (!Storage.CreateFile(tmp, len))
        return false;
    if (!Storage.WriteToFile(tmp, 0, len, (const char *)data))
    {
        Storage.DeleteFile(tmp); // don't leave an orphan staging file behind
        return false;
    }

    // Commit atomically: the staging file becomes the live backup under its final name.
    if (!Storage.RenameFile(tmp, name))
    {
        Storage.DeleteFile(tmp);
        return false;
    }
    return true;
}

// Reads a backup file into `out`; returns the byte count (0 if absent or too large).
inline uint16_t ReadBackupFile(const char name[8], uint8_t *out, uint16_t cap)
{
    return (uint16_t)Storage.ReadFromFile(name, 0, cap, (char *)out);
}

