#pragma once

// File-system layer (Docs/Services/Storage.md). Files are stored as 16-byte FileEntry
// records in a table page; a file's data occupies whole pages after it. One multi-file
// backend is shared by every target (Tamu and the DAS); only the geometry differs, via the
// per-device STORAGE_FLASH_SIZE/STORAGE_BLOCK_SIZE build flags.
//
// Split into parts, included here so the layer stays one translation unit:
//   StorageDefs.h      geometry, flash hooks, FileEntry and its helpers
//   StorageBlockFS.h   the full multi-file system

#include "Core/Functions/StorageDefs.h"

#include "Core/Functions/StorageBlockFS.h"
