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

// Serial-number database (mandatory for core, see Docs/Services/Device service.md).
// Single shared implementation: the registry lives in the reserved tail of the storage
// region (Storage_FlashReserve bytes, see Core/Functions/Storage.h) and is accessed through
// the Storage_Flash* functions, so no device-specific code is required.
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
    // Resets iteration to the first entry
    static void IterReset();
    // Fetches the next entry during iteration
    static bool IterNext(RegistryEntry &out_entry);

private:
    static void RecoverState();
    static uint16_t FindLowestAvailableID();
    static bool Compact();
    static bool IsFull();
    static bool RemoveDevice(uint16_t short_id);
    static bool Available();

    static uint32_t registry_offset; // Flash offset of the registry window within the storage region
    static uint32_t registry_size;   // Size of the registry window in bytes
    static uint32_t write_head;      // Write cursor (window-relative)
    static uint32_t read_tail;       // Oldest entry (window-relative)
    static int32_t active_count;     // Number of valid entries
    static size_t iter_pos;          // Iteration cursor (window-relative)
    static bool recovered;           // State already recovered from flash
};

uint32_t SNDB::registry_offset = 0;
uint32_t SNDB::registry_size = 0;
uint32_t SNDB::write_head = 0;
uint32_t SNDB::read_tail = 0;
int32_t SNDB::active_count = 0;
size_t SNDB::iter_pos = 0;
bool SNDB::recovered = false;

// Returns the registry window located at the end of the storage region. A device without a
// reserved tail (Storage_FlashReserve() == 0) has no registry and reports empty.
bool SNDB::Available()
{
    if (registry_size == 0 && Storage_FlashReserve() > 0)
    {
        registry_size = Storage_FlashReserve();
        registry_offset = (STORAGE_FLASH_SIZE >= registry_size)
                              ? (STORAGE_FLASH_SIZE - registry_size)
                              : 0;
    }
    return registry_size >= sizeof(RegistryEntry);
}

// Scans the registry to rebuild the write head, read tail and active device count after boot.
void SNDB::RecoverState()
{
    active_count = 0;
    size_t num_entries = registry_size / sizeof(RegistryEntry);

    size_t first_empty_idx = num_entries;
    size_t first_occ_after_empty_idx = num_entries;
    bool found_empty = false;

    for (size_t i = 0; i < num_entries; i++)
    {
        uint16_t state;
        Storage_FlashRead(registry_offset + i * sizeof(RegistryEntry), &state, sizeof(uint16_t));

        if (state == STATE_VALID)
            active_count++;

        if (state == STATE_EMPTY)
        {
            if (!found_empty)
            {
                first_empty_idx = i;
                found_empty = true;
            }
        }
        else
        {
            if (found_empty && first_occ_after_empty_idx == num_entries)
                first_occ_after_empty_idx = i;
        }
    }

    if (!found_empty)
    {
        read_tail = 0;
        write_head = 0;
    }
    else if (first_occ_after_empty_idx == num_entries)
    {
        read_tail = 0;
        write_head = first_empty_idx * sizeof(RegistryEntry);
    }
    else
    {
        read_tail = first_occ_after_empty_idx * sizeof(RegistryEntry);
        size_t last_empty_idx = first_occ_after_empty_idx - 1;
        while (last_empty_idx > first_empty_idx)
        {
            uint16_t state;
            Storage_FlashRead(registry_offset + last_empty_idx * sizeof(RegistryEntry), &state, sizeof(uint16_t));
            if (state != STATE_EMPTY) break;
            last_empty_idx--;
        }
        write_head = (last_empty_idx + 1) * sizeof(RegistryEntry);
    }

    DeviceLog("SNDB", "State Recovered. Head: %d, Tail: %d, Active Devices: %d",
              (int)write_head, (int)read_tail, (int)active_count);
}

// Walks the registry backwards from the write head looking for `serial`; returns its short ID or ADDR_INVALID.
uint16_t SNDB::FindShortID(const SerialNumber &serial)
{
    if (!Available())
        return ADDR_INVALID;
    if (!recovered)
    {
        RecoverState();
        recovered = true;
    }
    if (write_head == read_tail && active_count == 0)
        return ADDR_INVALID;

    size_t current = write_head;
    size_t num_entries = registry_size / sizeof(RegistryEntry);

    for (size_t i = 0; i < num_entries; i++)
    {
        if (current < sizeof(RegistryEntry))
            current = registry_size;
        current -= sizeof(RegistryEntry);

        RegistryEntry entry;
        Storage_FlashRead(registry_offset + current, &entry, sizeof(RegistryEntry));

        if (entry.valid == STATE_VALID)
        {
            if (memcmp(entry.uid.bytes, serial.bytes, 14) == 0)
                return entry.shortID;
        }
        if (current == read_tail) break;
    }
    return ADDR_INVALID;
}

// Registers a serial number with a short ID, removing any prior entry and compacting if full.
bool SNDB::AddDevice(const SerialNumber &serial, uint16_t short_id)
{
    if (!Available())
        return false;
    if (!recovered)
    {
        RecoverState();
        recovered = true;
    }

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

    if (Storage_FlashWrite(registry_offset + write_head, &new_entry, sizeof(RegistryEntry)))
    {
        write_head = (write_head + sizeof(RegistryEntry)) % registry_size;
        active_count++;
        return true;
    }
    return false;
}

// Relocates valid entries out of the block at the read tail, erases it, then advances the tail.
bool SNDB::Compact()
{
    while (IsFull())
    {
        size_t block_size = STORAGE_BLOCK_SIZE;
        size_t block_start = (read_tail / block_size) * block_size;

        if (write_head >= block_start && write_head < (block_start + block_size))
        {
            DeviceLog("SNDB", "Compaction deadlock! Write head inside target clean block.");
            return false;
        }

        for (size_t i = 0; i < (block_size / sizeof(RegistryEntry)); i++)
        {
            size_t addr = block_start + (i * sizeof(RegistryEntry));
            if (addr >= registry_size) break;

            RegistryEntry entry;
            Storage_FlashRead(registry_offset + addr, &entry, sizeof(RegistryEntry));

            if (entry.valid == STATE_VALID)
            {
                Storage_FlashWrite(registry_offset + write_head, &entry, sizeof(RegistryEntry));
                write_head = (write_head + sizeof(RegistryEntry)) % registry_size;
            }
        }

        Storage_FlashErase(registry_offset + block_start, block_size);
        read_tail = (block_start + block_size) % registry_size;
    }
    return true;
}

// Returns true when free space between the write head and read tail is below one erase block.
bool SNDB::IsFull()
{
    size_t free_space = 0;
    if (write_head >= read_tail)
    {
        free_space = registry_size - (write_head - read_tail);
        if (write_head == read_tail && active_count > 0)
            free_space = 0;
    }
    else
    {
        free_space = read_tail - write_head;
    }
    return free_space < STORAGE_BLOCK_SIZE;
}

// Marks every entry with the given short ID as removed (tombstone) and decrements the active count.
bool SNDB::RemoveDevice(uint16_t short_id)
{
    if (!Available())
        return false;
    if (!recovered)
    {
        RecoverState();
        recovered = true;
    }
    if (write_head == read_tail && active_count == 0)
        return false;

    size_t scan = read_tail;
    bool found = false;

    while (scan != write_head)
    {
        RegistryEntry entry;
        Storage_FlashRead(registry_offset + scan, &entry, sizeof(RegistryEntry));

        if (entry.valid == STATE_VALID && entry.shortID == short_id)
        {
            uint16_t tombstone = STATE_REMOVED;
            if (Storage_FlashWrite(registry_offset + scan, &tombstone, sizeof(uint16_t)))
            {
                active_count--;
                found = true;
            }
        }
        scan = (scan + sizeof(RegistryEntry)) % registry_size;
    }
    return found;
}

// Scans the registry for the lowest unused short ID starting from 2 (ID 1 is reserved for the core).
uint16_t SNDB::FindLowestAvailableID()
{
    uint16_t candidate = 2; // ID 1 reserved for Master/Core Node

    while (candidate < 0xFFFF)
    {
        bool collision = false;
        size_t scan = read_tail;

        while (scan != write_head)
        {
            RegistryEntry entry;
            Storage_FlashRead(registry_offset + scan, &entry, sizeof(RegistryEntry));

            if (entry.valid == STATE_VALID && entry.shortID == candidate)
            {
                collision = true;
                break;
            }
            scan = (scan + sizeof(RegistryEntry)) % registry_size;
        }

        if (!collision)
            return candidate;
        candidate++;
    }
    return ADDR_INVALID;
}

// Allocates the lowest available short ID for `serial` and registers it; returns the new ID or ADDR_INVALID.
uint16_t SNDB::NewDevice(const SerialNumber &serial)
{
    if (!Available())
        return ADDR_INVALID;
    if (!recovered)
    {
        RecoverState();
        recovered = true;
    }

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
    if (!Available())
        return false;
    if (!recovered)
    {
        RecoverState();
        recovered = true;
    }
    if (write_head == read_tail && active_count == 0)
        return false;

    size_t scan = read_tail;
    while (scan != write_head)
    {
        Storage_FlashRead(registry_offset + scan, &out_entry, sizeof(RegistryEntry));
        if (out_entry.valid == STATE_VALID && out_entry.shortID == short_id)
            return true;
        scan = (scan + sizeof(RegistryEntry)) % registry_size;
    }
    return false;
}

// Returns the number of active (registered) devices.
int32_t SNDB::ActiveCount()
{
    if (!Available())
        return 0;
    if (!recovered)
    {
        RecoverState();
        recovered = true;
    }
    return active_count;
}

// Resets the iteration cursor to the first (tail) registry entry.
void SNDB::IterReset()
{
    if (!Available())
        return;
    if (!recovered)
    {
        RecoverState();
        recovered = true;
    }
    iter_pos = read_tail;
}

// Returns the next valid registry entry via `out_entry`, or false when the head is reached.
bool SNDB::IterNext(RegistryEntry &out_entry)
{
    if (!Available())
        return false;
    if (!recovered)
    {
        RecoverState();
        recovered = true;
    }
    while (iter_pos != write_head)
    {
        Storage_FlashRead(registry_offset + iter_pos, &out_entry, sizeof(RegistryEntry));
        iter_pos = (iter_pos + sizeof(RegistryEntry)) % registry_size;
        if (out_entry.valid == STATE_VALID)
            return true;
    }
    return false;
}