#pragma once
#include "Log.h"
#include "esp_partition.h"

#define STORAGE_PARTITION_NAME "storage"

#include "Core/Services/Storage.h"

static const esp_partition_t *StoragePart = nullptr;

// Locates the SPIFFS "storage" partition and caches a handle to it; returns false if absent.
bool StorageFlashInit()
{
    StoragePart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                              ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                              STORAGE_PARTITION_NAME);
    if (!StoragePart) {
        DeviceLog("STORAGE", "Partition '%s' not found!", STORAGE_PARTITION_NAME);
        return false;
    }
    if (StoragePart->size != STORAGE_FLASH_SIZE) {
        DeviceLog("STORAGE", "Partition size %d does not match STORAGE_FLASH_SIZE %d!",
                  (int)StoragePart->size, (int)STORAGE_FLASH_SIZE);
        return false;
    }
    return true;
}

// Returns the size of the storage partition in bytes (0 if not initialised).
// Reads `size` bytes from the flash partition at `offset` into `data`.
uint32_t StorageFlashRead(uint32_t Offset, void *Data, uint32_t Size)
{
    if (!StoragePart || Size == 0) return 0;
    return (esp_partition_read(StoragePart, Offset, Data, Size) == ESP_OK) ? Size : 0;
}

// Writes `size` bytes from `data` to the flash partition at `offset`.
bool StorageFlashWrite(uint32_t Offset, const void *Data, uint32_t Size)
{
    if (!StoragePart) return false;
    return esp_partition_write(StoragePart, Offset, Data, Size) == ESP_OK;
}

// Erases `size` bytes in the flash partition starting at `offset`.
bool StorageFlashErase(uint32_t Offset, uint32_t Size)
{
    if (!StoragePart) return false;
    return esp_partition_erase_range(StoragePart, Offset, Size) == ESP_OK;
}

// Wipes the entire storage partition.
bool StorageFlashFormat()
{
    if (!StoragePart) return false;
    return esp_partition_erase_range(StoragePart, 0, StoragePart->size) == ESP_OK;
}