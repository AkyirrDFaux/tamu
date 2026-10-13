#pragma once

#include "Core/Functions/Packet.h"
#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Storage.h"
#include "Core/Functions/Log.h"

typedef struct __attribute__((packed, aligned(4)))
{
    uint16_t Valid;
    uint16_t ShortID;      // device-only id (0..1023)
    uint8_t Net;           // owning net: 0 = local net, else the other core's NetID
    uint8_t Reserved[11];  // net is carved out of the old 12 reserved bytes
    SerialNumber Uid;
} RegistryEntry;           // Total: 32 bytes (net does NOT grow the record)
static_assert(sizeof(RegistryEntry) == 32, "RegistryEntry layout must stay 32 Bytes");

// The wire address of a registry entry: MakeId(owning net, device id). Local devices carry
// net 0; other cores carry their NetID with device 1 (Docs/Services/System Block and Device
// Commands.md: "Local devices are stored with NetID 0. Other cores are stored only with their
// NetID.1.").
static inline uint16_t RegistryAddress(const RegistryEntry &Entry)
{
    return MakeId(Entry.Net, Entry.ShortID);
}

// A storable address is a local-net device (net 0, any device id) or a core (net != 0, device
// 1). Anything else is a device from a foreign net and is not stored, nor assigned an ID
// (Docs: "Devices from foreign nets are not stored").
static inline bool RegistryAddressStorable(uint16_t Address)
{
    return ((Address >> 10) & 0x3F) == 0 || (Address & 0x3FF) == 1;
}

#define STATE_EMPTY    0xFFFF
#define STATE_VALID    0x55AA
#define STATE_REMOVED  0x0000

// Registry capacity (entries). The registry file is created with this many 32-byte slots
// (SNDB_MAX_ENTRIES * 32 bytes). Override via build flag on RAM-starved devices.
#ifndef SNDB_MAX_ENTRIES
#define SNDB_MAX_ENTRIES 128
#endif

// Serial-number database (mandatory for core, see Docs/Services/System Block and Device Commands.md).
// Single shared implementation: the registry is a log-structured FILE in the storage
// filesystem (a file named "SNREG"), so no device-specific code and no reserved flash
// region is required. New entries are appended to the file, removed entries are
// tombstoned (valid 0x55AA -> 0x0000), and when the file fills up it is compacted by
// rewriting it with only the valid entries (delete + recreate + write).
class SNDB
{
public:
    // Looks up the net-qualified address (MakeId(net, device)) stored for a serial number,
    // or ADDR_INVALID.
    static uint16_t FindAddress(const SerialNumber &Serial);
    // Registers a new local-net device (net 0) and returns its assigned device ID
    static uint16_t NewDevice(const SerialNumber &Serial);
    // Adds a device with an explicit net-qualified address (local = net 0; other cores =
    // NetID.1). Foreign-net addresses are rejected.
    static bool AddDevice(const SerialNumber &Serial, uint16_t Address);
    // Reads the registry entry for a net-qualified address
    static bool GetEntry(uint16_t Address, RegistryEntry &OutEntry);
    // Tombstones every entry with the given net-qualified address (SNDB Delete)
    static bool RemoveDevice(uint16_t Address);
    // Resets iteration to the first entry
    static void IterReset();
    // Fetches the next entry during iteration
    static bool IterNext(RegistryEntry &OutEntry);
    // Number of valid entries in the recovered registry. Lets a caller size a stream
    // without a separate counting pass; it must be called after EnsureRecovered() (or
    // IterReset()) so the count reflects the file.
    static int32_t ActiveCount() { return EnsureRecovered() ? ActiveEntryCount : 0; }

private:
    static void RecoverState();
    static uint16_t FindLowestAvailableID();
    static bool Compact();
    static bool IsFull();
    static bool Available();
    // Ensures state was recovered from the file (shared prologue of every public method).
    static bool EnsureRecovered();

    // One registry entry read/written by index. The offset arithmetic lives here so it cannot
    // drift between the scan loops; the caller still decides what a failed read means (stop
    // the scan or skip the entry).
    static bool ReadEntry(const char *File, uint32_t Index, RegistryEntry &Out)
    {
        return Storage.ReadFromFile(File, Index * sizeof(RegistryEntry),
                                    sizeof(RegistryEntry), (char *)&Out) == sizeof(RegistryEntry);
    }
    static bool WriteEntry(const char *File, uint32_t Index, const void *Data, uint16_t Count)
    {
        return Storage.WriteToFile(File, Index * sizeof(RegistryEntry), Count, (const char *)Data);
    }

    // Calls `Fn(Index, Entry)` for every valid entry in the registry file, in file order.
    // `Fn` returns false to stop the scan early (an early return / first match); a failed read
    // ends the walk exactly as the old per-site `break` did. The five scans all walk the same
    // valid-entry set, so they share this walk instead of re-deriving the offset arithmetic.
    // Defined after the file-name helper (SNDBFileName) that its body calls.
    template <typename Fn>
    static void ForEachValid(Fn Visit);

    static uint32_t RegistrySize;   // Actual registry file size in bytes
    static uint32_t WriteHead;      // Next append offset (file-relative)
    static int32_t ActiveEntryCount;     // Number of valid entries
    static size_t IterPos;          // Iteration cursor (slot index)
    static bool Recovered;           // State already recovered from the file
    static RegistryEntry CompactBuf[SNDB_MAX_ENTRIES]; // Compaction scratch (dense rewrite)
};

uint32_t SNDB::RegistrySize = 0;
uint32_t SNDB::WriteHead = 0;
int32_t SNDB::ActiveEntryCount = 0;
size_t SNDB::IterPos = 0;
bool SNDB::Recovered = false;
RegistryEntry SNDB::CompactBuf[SNDB_MAX_ENTRIES] = {};

// Registry file name (8 plain-text characters, space padded).
static const char *SNDBFileName()
{
    static constexpr char Name[8] = {'.', 'S', 'N', 'R', 'E', 'G', ' ', ' '};
    return Name;
}

// Temporary file used during compaction: entries are staged here before the main
// registry is swapped, so a power loss mid-compaction never loses the registry
// (recovery rebuilds SNREG from the temp file).
static const char *SNDBTempName()
{
    static constexpr char Name[8] = {'S', 'N', 'R', 'T', 'M', 'P', ' ', ' '};
    return Name;
}

template <typename Fn>
void SNDB::ForEachValid(Fn Visit)
{
    uint32_t NumEntries = RegistrySize / sizeof(RegistryEntry);
    for (uint32_t i = 0; i < NumEntries; i++)
    {
        RegistryEntry Entry;
        if (!ReadEntry(SNDBFileName(), i, Entry)) return;
        if (Entry.Valid != STATE_VALID) continue;
        if (!Visit(i, Entry)) return;
    }
}

// Ensures the registry file exists (creating it on first use) and returns true when usable.
// After a Storage.Format() the file is gone and gets recreated here, so cached state is
// reset to force a fresh recovery scan. If a previous compaction was interrupted at ANY
// stage, the temp file is still present and holds the complete staged set (it is written
// before SNREG is touched), so the registry is rebuilt from it - whether SNREG is missing,
// freshly created, or partially rewritten.
bool SNDB::Available()
{
    uint32_t Sz = Storage.FileExists(SNDBFileName());
    uint32_t TempSz = Storage.FileExists(SNDBTempName());
    if (TempSz != 0xFFFFFFFF)
    {
        // Interrupted compaction: rebuild SNREG from the staged temp file. The temp
        // always contains the full set, so it is authoritative at every crash window.
        DeviceLog("SNDB", "Restoring registry from interrupted compaction");
        Storage.DeleteFile(SNDBFileName());
        bool RestoredOk = Storage.CreateFile(SNDBFileName(),
                                             SNDB_MAX_ENTRIES * sizeof(RegistryEntry));
        if (RestoredOk)
        {
            int32_t Restored = 0;
            uint32_t NumEntries = TempSz / sizeof(RegistryEntry);
            for (uint32_t i = 0; i < NumEntries && Restored < SNDB_MAX_ENTRIES; i++)
            {
                RegistryEntry Entry;
                if (!ReadEntry(SNDBTempName(), i, Entry))
                {
                    RestoredOk = false;
                    break;
                }
                if (Entry.Valid != STATE_VALID)
                    continue;
                if (!WriteEntry(SNDBFileName(), (uint32_t)Restored, &Entry, sizeof(RegistryEntry)))
                {
                    RestoredOk = false;
                    break;
                }
                Restored++;
            }
        }
        if (!RestoredOk)
        {
            // Keep the staged temp: the registry is not reliably rebuilt yet, so a
            // later boot retries from the intact copy instead of losing it.
            DeviceLog("SNDB", "Restore failed; keeping staged registry");
            return false;
        }
        Storage.DeleteFile(SNDBTempName());
        Recovered = false; // freshly written file: rescan
        WriteHead = 0;
        ActiveEntryCount = 0;
        IterPos = 0;
    }
    else if (Sz == 0xFFFFFFFF)
    {
        if (!Storage.CreateFile(SNDBFileName(), SNDB_MAX_ENTRIES * sizeof(RegistryEntry)))
        {
            DeviceLog("SNDB", "Registry file creation failed!");
            return false;
        }
        Recovered = false;   // freshly erased file: rescan
        WriteHead = 0;
        ActiveEntryCount = 0;
        IterPos = 0;
    }

    Sz = Storage.FileExists(SNDBFileName());
    if (Sz == 0xFFFFFFFF)
        return false;
    RegistrySize = Sz;
    return RegistrySize >= sizeof(RegistryEntry);
}

// Scans the registry file to rebuild the write head and the active device count after boot.
// The log is linear (appends always go forward, tombstones leave holes), so the first
// fully-erased (0xFF) slot is the append position. A torn append body (valid field still
// 0xFFFF but some data programmed) is skipped like a hole; compaction reclaims it.
void SNDB::RecoverState()
{
    ActiveEntryCount = 0;
    WriteHead = RegistrySize; // default: file full -> compact on next add
    uint32_t NumEntries = RegistrySize / sizeof(RegistryEntry);

    // Fully-erased slot pattern (all 0xFF). Built at runtime: an array initializer
    // `{0xFF}` would zero-fill the remaining bytes and never match.
    uint8_t Ff[sizeof(RegistryEntry)];
    memset(Ff, 0xFF, sizeof(Ff));
    for (uint32_t i = 0; i < NumEntries; i++)
    {
        RegistryEntry Entry;
        if (!ReadEntry(SNDBFileName(), i, Entry))
            break;
        if (memcmp(&Entry, Ff, sizeof(Entry)) == 0)
        {
            WriteHead = i * sizeof(RegistryEntry);
            break;
        }
        if (Entry.Valid == STATE_VALID)
            ActiveEntryCount++;
    }

    DeviceLog("SNDB", "State Recovered. Head: %d, Active Devices: %d",
              (int)WriteHead, (int)ActiveEntryCount);
}

// Shared prologue of every public method: makes sure the registry file is usable and
// its state has been recovered from flash.
bool SNDB::EnsureRecovered()
{
    if (!Available())
        return false;
    if (!Recovered)
    {
        RecoverState();
        Recovered = true;
    }
    return true;
}

// Walks the file looking for `serial`; returns its net-qualified stored address, or ADDR_INVALID.
uint16_t SNDB::FindAddress(const SerialNumber &Serial)
{
    if (!EnsureRecovered())
        return ADDR_INVALID;
    if (ActiveEntryCount == 0)
        return ADDR_INVALID;

    uint16_t Found = ADDR_INVALID;
    ForEachValid([&](uint32_t, const RegistryEntry &Entry) {
        if (memcmp(Entry.Uid.Bytes, Serial.Bytes, sizeof(Serial.Bytes)) == 0)
        {
            Found = RegistryAddress(Entry);
            return false;
        }
        return true;
    });
    return Found;
}

// Registers a serial number with a net-qualified address, removing any prior entry and
// compacting if full. Rejects foreign-net addresses (net != 0 and device != 1).
bool SNDB::AddDevice(const SerialNumber &Serial, uint16_t Address)
{
    if (!RegistryAddressStorable(Address))
        return false;
    // The core never stores itself (Docs/Services/System Block and Device Commands.md: "A core
    // does not store its own entry or its own NetID"). Short ID 1 is reserved for the core, so
    // the net-0 device-1 slot is always the core's own entry, never a storeable device; reject
    // it here so no write path can mint it. Other cores are stored as their NetID.1
    // (device 1 in a foreign net, a different address) and local devices start at ID 2.
    if (Address == MakeId(0, 1))
        return false;
    if (!EnsureRecovered())
        return false;

    uint16_t ExistingAddr = FindAddress(Serial);
    if (ExistingAddr != ADDR_INVALID)
        RemoveDevice(ExistingAddr);

    if (IsFull())
    {
        if (!Compact()) return false;
    }

    RegistryEntry NewEntry = {};
    NewEntry.Valid = STATE_VALID;
    NewEntry.ShortID = (uint16_t)(Address & 0x3FF);
    NewEntry.Net = (uint8_t)((Address >> 10) & 0x3F);
    NewEntry.Uid = Serial;
    memset(NewEntry.Reserved, 0, sizeof(NewEntry.Reserved));

    if (WriteHead + sizeof(RegistryEntry) > RegistrySize)
        return false;

    // Crash-safe append: write the body with an EMPTY marker first, then program the
    // VALID marker last. A torn write leaves the slot EMPTY and is ignored on recovery.
    RegistryEntry Body = NewEntry;
    Body.Valid = STATE_EMPTY;
    if (!Storage.WriteToFile(SNDBFileName(), WriteHead, sizeof(RegistryEntry),
                             (const char *)&Body))
        return false;
    uint16_t Marker = STATE_VALID;
    if (!Storage.WriteToFile(SNDBFileName(), WriteHead, sizeof(Marker),
                             (const char *)&Marker))
        return false;

    WriteHead += sizeof(RegistryEntry);
    ActiveEntryCount++;
    return true;
}

// Returns true when there is no room for another entry in the registry file.
bool SNDB::IsFull()
{
    return WriteHead + sizeof(RegistryEntry) > RegistrySize;
}

// Marks every entry with the given net-qualified address as removed (tombstone) and decrements the active count.
bool SNDB::RemoveDevice(uint16_t Address)
{
    if (!EnsureRecovered())
        return false;
    if (ActiveEntryCount == 0)
        return false;

    bool Found = false;
    ForEachValid([&](uint32_t i, const RegistryEntry &Entry) {
        if (RegistryAddress(Entry) == Address)
        {
            uint16_t Tombstone = STATE_REMOVED;
            if (WriteEntry(SNDBFileName(), i, &Tombstone, sizeof(uint16_t)))
            {
                ActiveEntryCount--;
                Found = true;
            }
        }
        return true;
    });
    return Found;
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
    uint32_t Count = 0;
    ForEachValid([&](uint32_t, const RegistryEntry &Entry) {
        if (Count < SNDB_MAX_ENTRIES)
            CompactBuf[Count++] = Entry;
        return true;
    });

    // 1. Stage all valid entries in the temp file (SNREG still intact).
    if (!Storage.DeleteFile(SNDBTempName()))
        return false;
    if (!Storage.CreateFile(SNDBTempName(), SNDB_MAX_ENTRIES * sizeof(RegistryEntry)))
        return false;
    for (uint32_t i = 0; i < Count; i++)
    {
        if (!Storage.WriteToFile(SNDBTempName(), i * sizeof(RegistryEntry),
                                 sizeof(RegistryEntry), (const char *)&CompactBuf[i]))
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
    bool Ok = true;
    for (uint32_t i = 0; i < Count && Ok; i++)
    {
        Ok = WriteEntry(SNDBFileName(), i, &CompactBuf[i], sizeof(RegistryEntry));
    }

    // 3. Compaction complete on this boot: drop the staging file only AFTER every
    //    entry was written - it is the intact copy if this boot loses power mid-swap.
    if (!Ok)
        return false; // temp file remains -> next boot restores from it
    Storage.DeleteFile(SNDBTempName());

    ActiveEntryCount = (int32_t)Count;
    WriteHead = Count * sizeof(RegistryEntry);
    RegistrySize = SNDB_MAX_ENTRIES * sizeof(RegistryEntry);
    IterPos = 0;
    Recovered = true;
    DeviceLog("SNDB", "Compacted, %d entries", (int)Count);
    return true;
}

// Scans the registry for the lowest unused short ID starting from 2 (ID 1 is reserved for the core).
uint16_t SNDB::FindLowestAvailableID()
{
    // One pass over the registry marks every used ID below ID_SCAN_LIMIT. The lowest
    // available ID can never exceed active_count + 2 <= SNDB_MAX_ENTRIES + 2, so the
    // limit comfortably covers all reachable candidates.
    const uint16_t ID_SCAN_LIMIT = 512;
    uint8_t Used[(ID_SCAN_LIMIT + 7) / 8] = {0};

    uint16_t Candidate = 2; // ID 1 reserved for Master/Core Node

    ForEachValid([&](uint32_t, const RegistryEntry &Entry) {
        if (Entry.ShortID < ID_SCAN_LIMIT)
        {
            uint16_t Id = Entry.ShortID;
            Used[Id >> 3] |= (uint8_t)(1u << (Id & 7));
        }
        return true;
    });

    while (Candidate < 0xFFFF)
    {
        if (Candidate >= ID_SCAN_LIMIT || !(Used[Candidate >> 3] & (1u << (Candidate & 7))))
            return Candidate;
        Candidate++;
    }
    return ADDR_INVALID;
}

// Allocates the lowest available short ID for `serial` and registers it; returns the new ID or ADDR_INVALID.
uint16_t SNDB::NewDevice(const SerialNumber &Serial)
{
    if (!EnsureRecovered())
        return ADDR_INVALID;

    uint16_t Id = FindLowestAvailableID();
    if (Id == ADDR_INVALID)
    {
        DeviceLog("SNDB", "Allocation failed: No IDs available");
        return ADDR_INVALID;
    }

    return AddDevice(Serial, MakeId(0, Id)) ? Id : ADDR_INVALID;
}

// Fills `out_entry` with the registry entry matching the net-qualified `address`.
bool SNDB::GetEntry(uint16_t Address, RegistryEntry &OutEntry)
{
    if (!EnsureRecovered())
        return false;
    if (ActiveEntryCount == 0)
        return false;

    bool Found = false;
    ForEachValid([&](uint32_t, const RegistryEntry &Entry) {
        if (RegistryAddress(Entry) == Address)
        {
            OutEntry = Entry;
            Found = true;
            return false;
        }
        return true;
    });
    return Found;
}

// Resets the iteration cursor to the first registry entry.
void SNDB::IterReset()
{
    if (!EnsureRecovered())
        return;
    IterPos = 0;
}

// Returns the next valid registry entry via `out_entry`, or false when the file is exhausted.
bool SNDB::IterNext(RegistryEntry &OutEntry)
{
    if (!EnsureRecovered())
        return false;

    uint32_t NumEntries = RegistrySize / sizeof(RegistryEntry);
    while (IterPos < NumEntries)
    {
        RegistryEntry Entry;
        if (!ReadEntry(SNDBFileName(), IterPos, Entry))
        {
            IterPos = NumEntries;
            return false;
        }
        IterPos++;
        if (Entry.Valid == STATE_VALID)
        {
            OutEntry = Entry;
            return true;
        }
    }
    return false;
}