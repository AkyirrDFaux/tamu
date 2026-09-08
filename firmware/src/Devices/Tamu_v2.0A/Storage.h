#pragma once
#include "Log.h"
#include "esp_partition.h"

#define STORAGE_PARTITION_NAME "storage"

#include "Core/Services/Storage.h"

static const esp_partition_t *g_storage_part = nullptr;

// Locates the SPIFFS "storage" partition and caches a handle to it; returns false if absent.
bool Storage_FlashInit()
{
    g_storage_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                              ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                              STORAGE_PARTITION_NAME);
    if (!g_storage_part) {
        DeviceLog("STORAGE", "Partition '%s' not found!", STORAGE_PARTITION_NAME);
        return false;
    }
    if (g_storage_part->size != STORAGE_FLASH_SIZE) {
        DeviceLog("STORAGE", "Partition size %d does not match STORAGE_FLASH_SIZE %d!",
                  (int)g_storage_part->size, (int)STORAGE_FLASH_SIZE);
        return false;
    }
    return true;
}

// Returns the size of the storage partition in bytes (0 if not initialised).
uint32_t Storage_FlashSize()
{
    return g_storage_part ? g_storage_part->size : 0;
}

// Reads `size` bytes from the flash partition at `offset` into `data`.
uint32_t Storage_FlashRead(uint32_t offset, void *data, uint32_t size)
{
    if (!g_storage_part || size == 0) return 0;
    return (esp_partition_read(g_storage_part, offset, data, size) == ESP_OK) ? size : 0;
}

// Writes `size` bytes from `data` to the flash partition at `offset`.
bool Storage_FlashWrite(uint32_t offset, const void *data, uint32_t size)
{
    if (!g_storage_part) return false;
    return esp_partition_write(g_storage_part, offset, data, size) == ESP_OK;
}

// Erases `size` bytes in the flash partition starting at `offset`.
bool Storage_FlashErase(uint32_t offset, uint32_t size)
{
    if (!g_storage_part) return false;
    return esp_partition_erase_range(g_storage_part, offset, size) == ESP_OK;
}

// Wipes the entire storage partition.
bool Storage_FlashFormat()
{
    if (!g_storage_part) return false;
    return esp_partition_erase_range(g_storage_part, 0, g_storage_part->size) == ESP_OK;
}