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

// Number of system-block fields. NetID (7) is Core-only (Docs: "applies only after
// reboot") and App Active (8) only exists on boards with an app interface, so a
// plain node like the DAS exposes fields 0-6.
#ifdef TYPE_CORE
#define SYSTEM_FIELD_COUNT 9
#else
#define SYSTEM_FIELD_COUNT 7
#endif

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

// Logical length of a STATLOG-style log: the entries up to the first 0xFF terminator (the
// format's end marker - an erased region is all 0xFF and means "empty").
//
// The raw file length is NOT that length. The reduced file system pre-allocates the settings
// file, so reading it returns the whole erased region (the DAS's STATLOG is 256 bytes of 0xFF);
// treating that as "used" made the append refuse with a full buffer, so *every* static save on
// the DAS failed (status 255, STATLOG untouched - found on the rig). The recall paths already
// stop at the terminator; this gives the save paths the same view.
static inline uint16_t BackupLogUsed(const uint8_t *buf, uint16_t len)
{
    uint16_t c = 0;
    while (c + kLogEntryHeaderSize <= len)
    {
        if (buf[c] == 0xFF) break; // terminator: the log ends here
        const uint8_t sz = buf[c + kLogEntryHeaderSize - 1];
        c = (uint16_t)(c + LogEntrySize(sz));
    }
    return c;
}

// ---- Active flags (Docs/Services/Register.md "Flag RAM") --------------------------------
// "Active flags have a (single, not split based of memory sub-type) paralel array of 4-bit
// segments. The blocks instances are stacked upon each other..., each block uses (4 * number
// of entries) bits. ... Total size of the RAM-array is (4 * number of all individual static
// entries) bits." The section is titled "System + Static memory blocks", so the System block
// is in the array too.
//
// Layout: block instances stack in static-registry order (which is the doc's type-then-
// instance order), each contributing 4 bits per schema entry, followed by the System block's
// segment. An entry's bit index is therefore
//     (sum of MapCount over the preceding registry entries + field) * 4 + flag
// The four bits are the four active flags, in the doc's table order.
enum ActiveFlag : uint8_t
{
    ActiveNotSaved = 0,        // a persistent field written but not saved
    ActiveScriptUpdated = 1,   // a script changed this field
    ActiveSubSource = 2,       // a subscription is reading this field
    ActiveExternal = 3,        // a subscription (or foreign script) updated this field
};

// Total entry slots the array must cover: every static-registry entry plus the System block.
// Sized per board (the registry's MapCounts are only visible to the board's own translation
// unit), so each board sets STATIC_ACTIVE_ENTRIES and asserts it fits.
#ifndef STATIC_ACTIVE_ENTRIES
#define STATIC_ACTIVE_ENTRIES 64
#endif
#define STATIC_ACTIVE_BITS_PER_ENTRY 4
#define STATIC_ACTIVE_BYTES (((size_t)STATIC_ACTIVE_ENTRIES * STATIC_ACTIVE_BITS_PER_ENTRY + 7) / 8)

// `inline` (not `static`): a `static` array in a header would give every translation unit its
// own copy of the flags. Same reason GetPI()/RandState() in Number.h are inline.
inline uint8_t StaticActiveBits[STATIC_ACTIVE_BYTES];

// Entry offset of registry block `idx`, field `field`: the doc's
// "block's instance * Number of entries in this Block Type + Position of entry" plus the
// preceding types' segments. Returns 0xFFFF when out of range.
// noinline: the registry walk would otherwise be duplicated into every set/get call site
// (the DAS pays for each copy).
__attribute__((noinline))
static uint16_t StaticEntryOffset(uint8_t idx, uint8_t field)
{
    // Registry blocks occupy the array in order; the System block's segment follows them all.
    const bool system = (idx == SYSTEM_BLOCK_BACKUP);
    if (!system && idx >= static_block_num) return 0xFFFF;

    uint16_t entry = 0;
    const size_t preceding = system ? static_block_num : idx;
    for (size_t i = 0; i < preceding; i++)
        entry = (uint16_t)(entry + static_block_registry[i].Schema->MapCount);

    const uint16_t entries = system ? SYSTEM_FIELD_COUNT : static_block_registry[idx].Schema->MapCount;
    if (field >= entries) return 0xFFFF;
    return (uint16_t)(entry + field);
}

static inline void StaticActiveSet(uint8_t idx, uint8_t field, ActiveFlag flag, bool value)
{
    const uint16_t entry = StaticEntryOffset(idx, field);
    if (entry == 0xFFFF || entry >= STATIC_ACTIVE_ENTRIES) return;
    const size_t bit = (size_t)entry * STATIC_ACTIVE_BITS_PER_ENTRY + (uint8_t)flag;
    if (value) StaticActiveBits[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    else StaticActiveBits[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

static inline bool StaticActiveGet(uint8_t idx, uint8_t field, ActiveFlag flag)
{
    const uint16_t entry = StaticEntryOffset(idx, field);
    if (entry == 0xFFFF || entry >= STATIC_ACTIVE_ENTRIES) return false;
    const size_t bit = (size_t)entry * STATIC_ACTIVE_BITS_PER_ENTRY + (uint8_t)flag;
    return (StaticActiveBits[bit >> 3] & (uint8_t)(1u << (bit & 7))) != 0;
}

// Not Saved is the one active flag with defined set/clear semantics today (write sets it,
// an explicit Save clears it); the other three are reserved pending their rules (see
// Issues.md "active flags").
static inline void StaticDirtySet(uint8_t idx, uint8_t field, bool dirty)
{
    StaticActiveSet(idx, field, ActiveNotSaved, dirty);
}

static inline bool StaticDirtyGet(uint8_t idx, uint8_t field)
{
    return StaticActiveGet(idx, field, ActiveNotSaved);
}

// Specification of the last write. The active flags record *what* last touched a value, so a
// write of a different specification clears the flags it does not match (user decision:
// "they clear when writing a new value of different specification - manual user's input vs
// local/foreign script").
enum WriteOrigin : uint8_t
{
    OriginManual = 0,      // the app, or any plain host write
    OriginLocalScript = 1, // one of this device's own scripts
    OriginForeign = 2,     // a subscription update, or another device's script
};

static inline void StaticActiveMarkWrite(uint8_t idx, uint8_t field, WriteOrigin origin)
{
#ifdef USE_SCRIPTS
    StaticActiveSet(idx, field, ActiveScriptUpdated, origin == OriginLocalScript);
#else
    // Without the Script service this flag can never be set, so there is nothing to clear.
    (void)origin;
#endif
    StaticActiveSet(idx, field, ActiveExternal, origin == OriginForeign);
}

// The active flags to fold into a read response - "a read combines the active and passive
// flags together" (Docs/Services/Register.md). Only the flags that have a wire bit are
// reported: the 6-bit flags field holds ReadOnly/Persistent/Trigger plus Not Saved, Script
// Updated and External origin, so the documented "Subscription Source" has no bit (Issues.md).
static inline uint16_t StaticActiveReported(uint8_t idx, uint8_t field)
{
    uint16_t f = 0;
    if (StaticActiveGet(idx, field, ActiveNotSaved)) f |= (uint16_t)FieldFlags::NotSaved;
#ifdef USE_SCRIPTS
    if (StaticActiveGet(idx, field, ActiveScriptUpdated)) f |= (uint16_t)FieldFlags::ScriptUpdated;
#endif
    if (StaticActiveGet(idx, field, ActiveExternal)) f |= (uint16_t)FieldFlags::External;
    return f;
}
