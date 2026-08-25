#pragma once

#include "Core/Functions/Packet.h"
#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Storage.h"
#include "Core/Functions/Log.h"

typedef struct __attribute__((packed, aligned(4)))
{
    uint16_t valid;
    uint16_t shortID;
    uint8_t reserved[12];
    SerialNumber uid;
} RegistryEntry;          // Total: 32 bytes

#define STATE_EMPTY    0xFFFF
#define STATE_VALID    0x55AA
#define STATE_REMOVED  0x0000

// Registry capacity (entries). The registry file is created with this many 32-byte slots
// (SNDB_MAX_ENTRIES * 32 bytes). Override via build flag on RAM-starved devices.
#ifndef SNDB_MAX_ENTRIES
#define SNDB_MAX_ENTRIES 128
#endif

// Serial-number database (mandatory for core, see Docs/Services/Device service.md).
// Single shared implementation: the registry is a log-structured FILE in the storage
// filesystem (a file named "SNREG"), so no device-specific code and no reserved flash
// region is required. New entries are appended to the file, removed entries are
// tombstoned (valid 0x55AA -> 0x0000), and when the file fills up it is compacted by
// rewriting it with only the valid entries (delete + recreate + write).
class SNDB
{
public:
    // Returns the number of registered devices
    static int32_t ActiveCount();
    // Looks up the short ID for a serial number
    static uint16_t FindShortID(const SerialNumber &serial);
    // Registers a new device and returns its assigned ID
    static uint16_t NewDevice(const SerialNumber &serial);
    // Adds a device with an explicit short ID
    static bool AddDevice(const SerialNumber &serial, uint16_t short_id);
    // Reads the registry entry for a short ID
    static bool GetEntry(uint16_t short_id, RegistryEntry &out_entry);
    // Tombstones every entry with the given short ID (SNDB Delete)
    static bool RemoveDevice(uint16_t short_id);
    // Resets iteration to the first entry
    static void IterReset();
    // Fetches the next entry during iteration
    static bool IterNext(RegistryEntry &out_entry);

private:
    static void RecoverState();
    static uint16_t FindLowestAvailableID();
    static bool Compact();
    static bool IsFull();
    static bool Available();
    // Ensures state was recovered from the file (shared prologue of every public method).
    static bool EnsureRecovered();

    static uint32_t registry_size;   // Actual registry file size in bytes
    static uint32_t write_head;      // Next append offset (file-relative)
    static int32_t active_count;     // Number of valid entries
    static size_t iter_pos;          // Iteration cursor (slot index)
    static bool recovered;           // State already recovered from the file
    static RegistryEntry compact_buf[SNDB_MAX_ENTRIES]; // Compaction scratch (dense rewrite)
};

uint32_t SNDB::registry_size = 0;
uint32_t SNDB::write_head = 0;
int32_t SNDB::active_count = 0;
size_t SNDB::iter_pos = 0;
bool SNDB::recovered = false;
RegistryEntry SNDB::compact_buf[SNDB_MAX_ENTRIES] = {};

// Registry file name (8 plain-text characters, space padded).
static const char *SNDBFileName()
{
    static constexpr char name[8] = {'S', 'N', 'R', 'E', 'G', ' ', ' ', ' '};
    return name;
}

// Temporary file used during compaction: entries are staged here before the main
// registry is swapped, so a power loss mid-compaction never loses the registry
// (recovery rebuilds SNREG from the temp file).
static const char *SNDBTempName()
{
    static constexpr char name[8] = {'S', 'N', 'R', 'T', 'M', 'P', ' ', ' '};
    return name;
}

// Ensures the registry file exists (creating it on first use) and returns true when usable.
// After a Storage.Format() the file is gone and gets recreated here, so cached state is
// reset to force a fresh recovery scan. If a previous compaction was interrupted (SNREG
// missing but the temp file present), the registry is rebuilt from the temp file.
bool SNDB::Available()
{
    uint32_t sz = Storage.FileExists(SNDBFileName());
    if (sz == 0xFFFFFFFF)
    {
        bool recreated = false;
        uint32_t temp_sz = Storage.FileExists(SNDBTempName());
        if (temp_sz != 0xFFFFFFFF)
        {
            // Interrupted compaction: restore every valid entry from the temp file.
            DeviceLog("SNDB", "Restoring registry from interrupted compaction");
            recreated = Storage.CreateFile(SNDBFileName(),
                                           SNDB_MAX_ENTRIES * sizeof(RegistryEntry));
            if (recreated)
            {
                int32_t restored = 0;
                uint32_t num_entries = temp_sz / sizeof(RegistryEntry);
                for (uint32_t i = 0; i < num_entries && restored < SNDB_MAX_ENTRIES; i++)
                {
                    RegistryEntry entry;
                    if (Storage.ReadFromFile(SNDBTempName(), i * sizeof(RegistryEntry),
                                             sizeof(entry), (char *)&entry) != sizeof(entry))
                        break;
                    if (entry.valid != STATE_VALID)
                        continue;
                    Storage.WriteToFile(SNDBFileName(),
                                        (uint32_t)restored * sizeof(RegistryEntry),
                                        sizeof(RegistryEntry), (const char *)&entry);
                    restored++;
                }
                recovered = false; // freshly written file: rescan
                write_head = 0;
                active_count = 0;
                iter_pos = 0;
            }
            Storage.DeleteFile(SNDBTempName());
        }

        if (!recreated)
        {
            if (!Storage.CreateFile(SNDBFileName(), SNDB_MAX_ENTRIES * sizeof(RegistryEntry)))
            {
                DeviceLog("SNDB", "Registry file creation failed!");
                return false;
            }
            recovered = false;   // freshly erased file: rescan
            write_head = 0;
            active_count = 0;
            iter_pos = 0;
        }

        sz = Storage.FileExists(SNDBFileName());
        if (sz == 0xFFFFFFFF)
            return false;
    }
    registry_size = sz;
    return registry_size >= sizeof(RegistryEntry);
}

// Scans the registry file to rebuild the write head and the active device count after boot.
// The log is linear (appends always go forward, tombstones leave holes), so the first
// fully-erased (0xFF) slot is the append position. A torn append body (valid field still
// 0xFFFF but some data programmed) is skipped like a hole; compaction reclaims it.
void SNDB::RecoverState()
{
    active_count = 0;
    write_head = registry_size; // default: file full -> compact on next add
    uint32_t num_entries = registry_size / sizeof(RegistryEntry);

    // Fully-erased slot pattern (all 0xFF). Built at runtime: an array initializer
    // `{0xFF}` would zero-fill the remaining bytes and never match.
    uint8_t ff[sizeof(RegistryEntry)];
    memset(ff, 0xFF, sizeof(ff));
    for (uint32_t i = 0; i < num_entries; i++)
    {
        RegistryEntry entry;
        if (Storage.ReadFromFile(SNDBFileName(), i * sizeof(RegistryEntry),
                                 sizeof(entry), (char *)&entry) != sizeof(entry))
            break;
        if (memcmp(&entry, ff, sizeof(entry)) == 0)
        {
            write_head = i * sizeof(RegistryEntry);
            break;
        }
        if (entry.valid == STATE_VALID)
            active_count++;
    }

    DeviceLog("SNDB", "State Recovered. Head: %d, Active Devices: %d",
              (int)write_head, (int)active_count);
}

// Shared prologue of every public method: makes sure the registry file is usable and
// its state has been recovered from flash.
bool SNDB::EnsureRecovered()
{
    if (!Available())
        return false;
    if (!recovered)
    {
        RecoverState();
        recovered = true;
    }
    return true;
}

// Walks the file looking for `serial`; returns its short ID or ADDR_INVALID.
uint16_t SNDB::FindShortID(const SerialNumber &serial)
{
    if (!EnsureRecovered())
        return ADDR_INVALID;
    if (active_count == 0)
        return ADDR_INVALID;

    uint32_t num_entries = registry_size / sizeof(RegistryEntry);
    for (uint32_t i = 0; i < num_entries; i++)
    {
        RegistryEntry entry;
        if (Storage.ReadFromFile(SNDBFileName(), i * sizeof(RegistryEntry),
                                 sizeof(entry), (char *)&entry) != sizeof(entry))
            break;
        if (entry.valid == STATE_VALID &&
            memcmp(entry.uid.bytes, serial.bytes, sizeof(serial.bytes)) == 0)
            return entry.shortID;
    }
    return ADDR_INVALID;
}

// Registers a serial number with a short ID, removing any prior entry and compacting if full.
bool SNDB::AddDevice(const SerialNumber &serial, uint16_t short_id)
{
    if (!EnsureRecovered())
        return false;

    uint16_t existing_id = FindShortID(serial);
    if (existing_id != ADDR_INVALID)
        RemoveDevice(existing_id);

    if (IsFull())
    {
        if (!Compact()) return false;
    }

    RegistryEntry new_entry = {};
    new_entry.valid = STATE_VALID;
    new_entry.shortID = short_id;
    new_entry.uid = serial;
    memset(new_entry.reserved, 0, 12);

    if (write_head + sizeof(RegistryEntry) > registry_size)
        return false;

    // Crash-safe append: write the body with an EMPTY marker first, then program the
    // VALID marker last. A torn write leaves the slot EMPTY and is ignored on recovery.
    RegistryEntry body = new_entry;
    body.valid = STATE_EMPTY;
    if (!Storage.WriteToFile(SNDBFileName(), write_head, sizeof(RegistryEntry),
                             (const char *)&body))
        return false;
    uint16_t marker = STATE_VALID;
    if (!Storage.WriteToFile(SNDBFileName(), write_head, sizeof(marker),
                             (const char *)&marker))
        return false;

    write_head += sizeof(RegistryEntry);
    active_count++;
    return true;
}

// Returns true when there is no room for another entry in the registry file.
bool SNDB::IsFull()
{
    return write_head + sizeof(RegistryEntry) > registry_size;
}

// Marks every entry with the given short ID as removed (tombstone) and decrements the active count.
bool SNDB::RemoveDevice(uint16_t short_id)
{
    if (!EnsureRecovered())
        return false;
    if (active_count == 0)
        return false;

    uint32_t num_entries = registry_size / sizeof(RegistryEntry);
    bool found = false;
    for (uint32_t i = 0; i < num_entries; i++)
    {
        RegistryEntry entry;
        if (Storage.ReadFromFile(SNDBFileName(), i * sizeof(RegistryEntry),
                                 sizeof(entry), (char *)&entry) != sizeof(entry))
            break;
        if (entry.valid == STATE_VALID && entry.shortID == short_id)
        {
            uint16_t tombstone = STATE_REMOVED;
            if (Storage.WriteToFile(SNDBFileName(), i * sizeof(RegistryEntry),
                                    sizeof(uint16_t), (const char *)&tombstone))
            {
                active_count--;
                found = true;
            }
        }
    }
    return found;
}

// Rewrites the registry file densely with only the valid entries. Called when the log is
// full of entries/tombstones.
//
// Crash-safe order: the valid entries are staged into a temp file first while SNREG stays
// untouched, then SNREG is swapped for a fresh file and rewritten from RAM. A power loss
// before the delete leaves both files intact; a power loss after it is repaired by
// Available(), which rebuilds SNREG from the temp file.
bool SNDB::Compact()
{
    uint32_t count = 0;
    uint32_t num_entries = registry_size / sizeof(RegistryEntry);
    for (uint32_t i = 0; i < num_entries; i++)
    {
        RegistryEntry entry;
        if (Storage.ReadFromFile(SNDBFileName(), i * sizeof(RegistryEntry),
                                 sizeof(entry), (char *)&entry) != sizeof(entry))
            break;
        if (entry.valid == STATE_VALID && count < SNDB_MAX_ENTRIES)
            compact_buf[count++] = entry;
    }

    // 1. Stage all valid entries in the temp file (SNREG still intact).
    if (!Storage.DeleteFile(SNDBTempName()))
        return false;
    if (!Storage.CreateFile(SNDBTempName(), SNDB_MAX_ENTRIES * sizeof(RegistryEntry)))
        return false;
    for (uint32_t i = 0; i < count; i++)
    {
        if (!Storage.WriteToFile(SNDBTempName(), i * sizeof(RegistryEntry),
                                 sizeof(RegistryEntry), (const char *)&compact_buf[i]))
        {
            Storage.DeleteFile(SNDBTempName());
            return false;
        }
    }

    // 2. Swap: replace SNREG with a fresh file and write the staged entries back.
    if (!Storage.DeleteFile(SNDBFileName()))
        return false; // temp file remains -> next boot restores from it
    if (!Storage.CreateFile(SNDBFileName(), SNDB_MAX_ENTRIES * sizeof(RegistryEntry)))
        return false;
    bool ok = true;
    for (uint32_t i = 0; i < count && ok; i++)
    {
        ok = Storage.WriteToFile(SNDBFileName(), i * sizeof(RegistryEntry),
                                 sizeof(RegistryEntry), (const char *)&compact_buf[i]);
    }

    // 3. Compaction complete on this boot: drop the staging file.
    Storage.DeleteFile(SNDBTempName());
    if (!ok)
        return false;

    active_count = (int32_t)count;
    write_head = count * sizeof(RegistryEntry);
    registry_size = SNDB_MAX_ENTRIES * sizeof(RegistryEntry);
    iter_pos = 0;
    recovered = true;
    DeviceLog("SNDB", "Compacted, %d entries", (int)count);
    return true;
}

// Scans the registry for the lowest unused short ID starting from 2 (ID 1 is reserved for the core).
uint16_t SNDB::FindLowestAvailableID()
{
    // One pass over the registry marks every used ID below ID_SCAN_LIMIT. The lowest
    // available ID can never exceed active_count + 2 <= SNDB_MAX_ENTRIES + 2, so the
    // limit comfortably covers all reachable candidates.
    const uint16_t ID_SCAN_LIMIT = 512;
    uint8_t used[(ID_SCAN_LIMIT + 7) / 8] = {0};

    uint16_t candidate = 2; // ID 1 reserved for Master/Core Node

    uint32_t num_entries = registry_size / sizeof(RegistryEntry);
    for (uint32_t i = 0; i < num_entries; i++)
    {
        RegistryEntry entry;
        if (Storage.ReadFromFile(SNDBFileName(), i * sizeof(RegistryEntry),
                                 sizeof(entry), (char *)&entry) != sizeof(entry))
            break;
        if (entry.valid == STATE_VALID && entry.shortID < ID_SCAN_LIMIT)
        {
            uint16_t id = entry.shortID;
            used[id >> 3] |= (uint8_t)(1u << (id & 7));
        }
    }

    while (candidate < 0xFFFF)
    {
        if (candidate >= ID_SCAN_LIMIT || !(used[candidate >> 3] & (1u << (candidate & 7))))
            return candidate;
        candidate++;
    }
    return ADDR_INVALID;
}

// Allocates the lowest available short ID for `serial` and registers it; returns the new ID or ADDR_INVALID.
uint16_t SNDB::NewDevice(const SerialNumber &serial)
{
    if (!EnsureRecovered())
        return ADDR_INVALID;

    uint16_t id = FindLowestAvailableID();
    if (id == ADDR_INVALID)
    {
        DeviceLog("SNDB", "Allocation failed: No IDs available");
        return ADDR_INVALID;
    }

    return AddDevice(serial, id) ? id : ADDR_INVALID;
}

// Fills `out_entry` with the registry entry matching `short_id`; returns false if not found.
bool SNDB::GetEntry(uint16_t short_id, RegistryEntry &out_entry)
{
    if (!EnsureRecovered())
        return false;
    if (active_count == 0)
        return false;

    uint32_t num_entries = registry_size / sizeof(RegistryEntry);
    for (uint32_t i = 0; i < num_entries; i++)
    {
        RegistryEntry entry;
        if (Storage.ReadFromFile(SNDBFileName(), i * sizeof(RegistryEntry),
                                 sizeof(entry), (char *)&entry) != sizeof(entry))
            break;
        if (entry.valid == STATE_VALID && entry.shortID == short_id)
        {
            out_entry = entry;
            return true;
        }
    }
    return false;
}

// Returns the number of active (registered) devices.
int32_t SNDB::ActiveCount()
{
    if (!EnsureRecovered())
        return 0;
    return active_count;
}

// Resets the iteration cursor to the first registry entry.
void SNDB::IterReset()
{
    if (!EnsureRecovered())
        return;
    iter_pos = 0;
}

// Returns the next valid registry entry via `out_entry`, or false when the file is exhausted.
bool SNDB::IterNext(RegistryEntry &out_entry)
{
    if (!EnsureRecovered())
        return false;

    uint32_t num_entries = registry_size / sizeof(RegistryEntry);
    while (iter_pos < num_entries)
    {
        RegistryEntry entry;
        if (Storage.ReadFromFile(SNDBFileName(), iter_pos * sizeof(RegistryEntry),
                                 sizeof(entry), (char *)&entry) != sizeof(entry))
        {
            iter_pos = num_entries;
            return false;
        }
        iter_pos++;
        if (entry.valid == STATE_VALID)
        {
            out_entry = entry;
            return true;
        }
    }
    return false;
}