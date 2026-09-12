#pragma once

#include "Core/Functions/Memory.h"

static const char *StaticLogName()
{
    static constexpr char name[8] = {'S', 'T', 'A', 'T', 'L', 'O', 'G', ' '};
    return name;
}

// The System block (type 0, inst 0) has no static-registry index; its persistent
// fields (Name 6, NetID 7) are stored in the same STATLOG mirror under this reserved
// block marker (0xFF is the end-of-log sentinel, registry indices are 0..N).
#define SYSTEM_BLOCK_BACKUP 0xFE
#define SYSTEM_FIELD_NAME   6
#define SYSTEM_FIELD_NETID  7

// BlockLog: sequential log file for persistent field-level storage.
// Caller provides buffer, zero internal allocations.
// Entry format: BlockIndex[4B] + BlockMeta[4B] + Value[NB, 4B-padded]
// BlockIndex.block == 0xFF  => unwritten (end of log)
// BlockIndex.block == 0x00  => invalidated entry

static constexpr uint16_t kLogEntryHeaderSize = sizeof(BlockMeta) + sizeof(BlockIndex);

static inline uint16_t LogEntrySize(uint8_t value_size)
{
    return kLogEntryHeaderSize + ((value_size + 3) & ~3);
}