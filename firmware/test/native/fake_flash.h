// The fake 512 B NOR store the DAS runs on (8 x 64 B pages), shared by the two storage tests.
//
// The geometry comes from the build flags the test runner passes (STORAGE_FLASH_SIZE=512,
// STORAGE_BLOCK_SIZE=64, MEMORY_BACKUP_CAP=128), so the layout exercised here is the layout the
// node runs. NOR semantics are modelled on purpose: a program only clears bits (1 -> 0), so a
// write onto a page that was not erased shows up as a wrong read-back instead of being silently
// accepted, and a missing erase cannot pass a test.
//
// Include this AFTER the storage headers - it defines members of StorageFlashDevice.
#pragma once

#include <cstdint>
#include <cstring>

static uint8_t Flash[STORAGE_FLASH_SIZE];

bool StorageFlashDevice::Init()
{
    memset(Flash, 0xFF, sizeof(Flash));
    return true;
}

uint32_t StorageFlashDevice::Read(uint32_t Offset, void *Data, uint32_t Size)
{
    if (Offset > STORAGE_FLASH_SIZE || Size > STORAGE_FLASH_SIZE - Offset) return 0;
    memcpy(Data, Flash + Offset, Size);
    return Size;
}

bool StorageFlashDevice::Write(uint32_t Offset, const void *Data, uint32_t Size)
{
    if (Offset > STORAGE_FLASH_SIZE || Size > STORAGE_FLASH_SIZE - Offset) return false;
    const uint8_t *Src = (const uint8_t *)Data;
    for (uint32_t i = 0; i < Size; i++)
        Flash[Offset + i] &= Src[i];
    return true;
}

bool StorageFlashDevice::Erase(uint32_t Offset, uint32_t Size)
{
    if ((Offset & (STORAGE_BLOCK_SIZE - 1)) || (Size & (STORAGE_BLOCK_SIZE - 1))) return false;
    if (Offset > STORAGE_FLASH_SIZE || Size > STORAGE_FLASH_SIZE - Offset) return false;
    memset(Flash + Offset, 0xFF, Size);
    return true;
}

bool StorageFlashDevice::Format() { return Erase(0, STORAGE_FLASH_SIZE); }
